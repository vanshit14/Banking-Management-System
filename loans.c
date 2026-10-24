#define _GNU_SOURCE
#include "loans.h"
#include "common.h"
#include "fileio.h"
#include "accounts.h"
#include "transactions.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MAX_LOAN_AMOUNT 1000000000000000LL

static int valid_loan(const LoanRecord *loan) {
    return loan->loan_id > 0 && loan->customer_id > 0 &&
           loan->amount > 0 && loan->amount <= MAX_LOAN_AMOUNT &&
           loan->status >= LOAN_PENDING && loan->status <= LOAN_REJECTED &&
           loan->assigned_employee_id >= 0;
}

const char *loan_status_name(LoanStatus status) {
    switch (status) {
        case LOAN_PENDING:  return "PENDING";
        case LOAN_IN_REVIEW:return "IN_REVIEW";
        case LOAN_APPROVED: return "APPROVED";
        case LOAN_REJECTED: return "REJECTED";
        default:            return "UNKNOWN";
    }
}

static LoanStatus parse_status(const char *text) {
    if (strcmp(text, "PENDING") == 0) return LOAN_PENDING;
    if (strcmp(text, "IN_REVIEW") == 0) return LOAN_IN_REVIEW;
    if (strcmp(text, "APPROVED") == 0) return LOAN_APPROVED;
    if (strcmp(text, "REJECTED") == 0) return LOAN_REJECTED;
    return 0;
}

static int parse_legacy_loan(char *line, LoanRecord *loan) {
    char status[32], extra;
    long long amount = 0;
    int matched = sscanf(line, "%d|%d|%lld|%31[^|]|%d %c",
                         &loan->loan_id, &loan->customer_id, &amount,
                         status, &loan->assigned_employee_id, &extra);
    loan->amount = amount;
    loan->status = matched == 5 ? parse_status(status) : 0;
    return matched == 5 && valid_loan(loan);
}

int init_loans(void) {
    int fd = open(LOANS_FILE, O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
        off_t size = lseek(fd, 0, SEEK_END);
        close(fd);
        return size >= 0 && size % (off_t)sizeof(LoanRecord) == 0 ? 0 : -1;
    }
    if (errno != ENOENT)
        return -1;

    int input = open(LEGACY_LOANS_FILE, O_RDONLY | O_CLOEXEC);
    if (input < 0)
        return -1;
    int output = open(LOANS_TEMP_FILE,
                      O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (output < 0) {
        close(input);
        return -1;
    }

    int result = -1, eof = 0;
    char line[BUFFER_SIZE];
    while (1) {
        int rc = read_line_fd(input, line, sizeof(line), &eof);
        if (rc == 0)
            break;
        if (rc < 0)
            goto done;
        LoanRecord loan;
        if (!parse_legacy_loan(line, &loan) ||
            write_all(output, (const char *)&loan, sizeof(loan)) < 0)
            goto done;
        if (eof)
            break;
    }
    if (fsync(output) < 0)
        goto done;
    if (close(output) < 0) {
        output = -1;
        goto done;
    }
    output = -1;
    if (rename(LOANS_TEMP_FILE, LOANS_FILE) < 0 ||
        fsync_path_dir("data") < 0)
        goto done;
    result = 0;

done:
    if (output >= 0)
        close(output);
    close(input);
    if (result != 0)
        unlink(LOANS_TEMP_FILE);
    return result;
}

int apply_loan(int user_id, long long amount, int *loan_id) {
    if (user_id <= 0 || amount <= 0 || amount > MAX_LOAN_AMOUNT || !loan_id)
        return -1;
    int structure_lock = ofd_lock(LOANS_STRUCTURE_LOCK_FILE, F_WRLCK);
    if (structure_lock < 0)
        return -1;
    int fd = open(LOANS_FILE, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        ofd_unlock(structure_lock);
        return -1;
    }

    int max_id = 0, result = -1;
    int account_id, active;
    long long balance;
    if (get_account_by_owner(user_id, &account_id, &balance, &active) != 1 ||
        !active) goto done;
    off_t offset = 0;
    while (1) {
        if (lock_region(fd, F_RDLCK, offset, sizeof(LoanRecord)) < 0)
            goto done;
        LoanRecord loan;
        int rc = pread_all(fd, &loan, sizeof(loan), offset);
        if (unlock_region(fd, offset, sizeof(LoanRecord)) < 0)
            goto done;
        if (rc == 0)
            break;
        if (rc < 0 || !valid_loan(&loan))
            goto done;
        if (loan.loan_id > max_id)
            max_id = loan.loan_id;
        offset += (off_t)sizeof(LoanRecord);
    }
    if (max_id == 2147483647)
        goto done;

    LoanRecord loan = {
        .loan_id = max_id + 1, .customer_id = user_id, .amount = amount,
        .status = LOAN_PENDING, .assigned_employee_id = 0
    };
    if (lock_region(fd, F_WRLCK, offset, sizeof(LoanRecord)) < 0)
        goto done;
    if (pwrite_all(fd, &loan, sizeof(loan), offset) == 0 && fsync(fd) == 0) {
        *loan_id = loan.loan_id;
        result = 1;
    } else {
        if (ftruncate(fd, offset) < 0) result = -1;
        (void)fsync(fd);
    }
    (void)unlock_region(fd, offset, sizeof(LoanRecord));

done:
    close(fd);
    ofd_unlock(structure_lock);
    return result;
}

int assign_loan(int loan_id, int employee_id) {
    if (loan_id <= 0 || employee_id <= 0)
        return -1;
    int fd = open(LOANS_FILE, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -1;

    int result = 0;
    off_t target = -1;
    for (off_t offset = 0;; offset += (off_t)sizeof(LoanRecord)) {
        if (lock_region(fd, F_RDLCK, offset, sizeof(LoanRecord)) < 0) {
            result = -1;
            break;
        }
        LoanRecord loan;
        int rc = pread_all(fd, &loan, sizeof(loan), offset);
        if (unlock_region(fd, offset, sizeof(LoanRecord)) < 0) {
            result = -1;
            break;
        }
        if (rc == 0)
            break;
        if (rc < 0 || !valid_loan(&loan)) {
            result = -1;
            break;
        }
        if (loan.loan_id == loan_id) {
            target = offset;
            break;
        }
    }

    if (result == 0 && target >= 0) {
        if (lock_region(fd, F_WRLCK, target, sizeof(LoanRecord)) < 0) {
            result = -1;
        } else {
            LoanRecord loan;
            if (pread_all(fd, &loan, sizeof(loan), target) != 1 ||
                !valid_loan(&loan) || loan.loan_id != loan_id) {
                result = -1;
            } else if (loan.status != LOAN_PENDING ||
                       loan.assigned_employee_id != 0) {
                result = -2;
            } else {
                loan.assigned_employee_id = employee_id;
                result = pwrite_all(fd, &loan, sizeof(loan), target) == 0 &&
                         fsync(fd) == 0 ? 1 : -1;
            }
            if (unlock_region(fd, target, sizeof(LoanRecord)) < 0 && result == 1)
                result = -1;
        }
    }
    close(fd);
    return result;
}

int list_assigned_loans(int employee_id, LoanRecord *loans,
                        int max_loans, int *count) {
    if (employee_id <= 0 || !loans || max_loans <= 0 || !count)
        return -1;
    *count = 0;
    int fd = open(LOANS_FILE, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;

    int result = 1;
    for (off_t offset = 0;; offset += (off_t)sizeof(LoanRecord)) {
        if (lock_region(fd, F_RDLCK, offset, sizeof(LoanRecord)) < 0) {
            result = -1;
            break;
        }
        LoanRecord loan;
        int rc = pread_all(fd, &loan, sizeof(loan), offset);
        if (unlock_region(fd, offset, sizeof(LoanRecord)) < 0) {
            result = -1;
            break;
        }
        if (rc == 0)
            break;
        if (rc < 0 || !valid_loan(&loan)) {
            result = -1;
            break;
        }
        if (loan.assigned_employee_id == employee_id) {
            if (*count >= max_loans) {
                result = -1;
                break;
            }
            loans[*count] = loan;
            (*count)++;
        }
    }
    close(fd);
    return result;
}

int list_unassigned_loans(LoanRecord *loans, int max_loans, int *count) {
    if (!loans || max_loans <= 0 || !count)
        return -1;
    *count = 0;
    int fd = open(LOANS_FILE, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;

    int result = 1;
    for (off_t offset = 0;; offset += (off_t)sizeof(LoanRecord)) {
        if (lock_region(fd, F_RDLCK, offset, sizeof(LoanRecord)) < 0) {
            result = -1;
            break;
        }
        LoanRecord loan;
        int rc = pread_all(fd, &loan, sizeof(loan), offset);
        if (unlock_region(fd, offset, sizeof(LoanRecord)) < 0) {
            result = -1;
            break;
        }
        if (rc == 0)
            break;
        if (rc < 0 || !valid_loan(&loan)) {
            result = -1;
            break;
        }
        if (loan.status == LOAN_PENDING && loan.assigned_employee_id == 0) {
            if (*count >= max_loans) {
                result = -1;
                break;
            }
            loans[*count] = loan;
            (*count)++;
        }
    }
    close(fd);
    return result;
}

int start_loan_processing(int loan_id, int employee_id) {
    if (loan_id <= 0 || employee_id <= 0)
        return -1;
    int fd = open(LOANS_FILE, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -1;

    int result = 0;
    off_t target = -1;
    for (off_t offset = 0;; offset += (off_t)sizeof(LoanRecord)) {
        if (lock_region(fd, F_RDLCK, offset, sizeof(LoanRecord)) < 0) {
            result = -1;
            break;
        }
        LoanRecord loan;
        int rc = pread_all(fd, &loan, sizeof(loan), offset);
        if (unlock_region(fd, offset, sizeof(LoanRecord)) < 0) {
            result = -1;
            break;
        }
        if (rc == 0)
            break;
        if (rc < 0 || !valid_loan(&loan)) {
            result = -1;
            break;
        }
        if (loan.loan_id == loan_id) {
            target = offset;
            break;
        }
    }

    if (result == 0 && target >= 0) {
        if (lock_region(fd, F_WRLCK, target, sizeof(LoanRecord)) < 0) {
            result = -1;
        } else {
            LoanRecord loan;
            if (pread_all(fd, &loan, sizeof(loan), target) != 1 ||
                !valid_loan(&loan) || loan.loan_id != loan_id) {
                result = -1;
            } else if (loan.assigned_employee_id != employee_id) {
                result = -2;
            } else if (loan.status != LOAN_PENDING) {
                result = -3;
            } else {
                loan.status = LOAN_IN_REVIEW;
                result = pwrite_all(fd, &loan, sizeof(loan), target) == 0 &&
                         fsync(fd) == 0 ? 1 : -1;
            }
            if (unlock_region(fd, target, sizeof(LoanRecord)) < 0 && result == 1)
                result = -1;
        }
    }
    close(fd);
    return result;
}

/* Caller holds the loan write lock. Keep the account locked until both
 * records and the passbook entry have been written. Lock order: loan, account,
 * transaction log, matching banking operations' account -> log order. */
static int disburse_loan(int loan_fd, off_t loan_offset, LoanRecord *loan) {
    int fd = open(ACCOUNTS_FILE, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -1;
    int result = -4;
    for (off_t offset = 0;; offset += (off_t)sizeof(AccountRecord)) {
        if (lock_region(fd, F_WRLCK, offset, sizeof(AccountRecord)) < 0) {
            result = -1;
            break;
        }
        AccountRecord original;
        int rc = pread_all(fd, &original, sizeof(original), offset);
        if (rc != 1) {
            result = rc == 0 ? -4 : -1;
            (void)unlock_region(fd, offset, sizeof(AccountRecord));
            break;
        }
        if (original.owner_id != loan->customer_id) {
            if (unlock_region(fd, offset, sizeof(AccountRecord)) < 0) {
                result = -1;
                break;
            }
            continue;
        }
        if (original.account_id <= 0 || original.active != 1 ||
            original.balance < 0 || original.balance > MAX_LOAN_AMOUNT ||
            loan->amount > MAX_LOAN_AMOUNT - original.balance) {
            (void)unlock_region(fd, offset, sizeof(AccountRecord));
            break;
        }
        AccountRecord credited = original;
        LoanRecord approved = *loan;
        credited.balance += loan->amount;
        approved.status = LOAN_APPROVED;
        result = -1;
        if (pwrite_all(fd, &credited, sizeof(credited), offset) == 0 &&
            fsync(fd) == 0 &&
            pwrite_all(loan_fd, &approved, sizeof(approved), loan_offset) == 0 &&
            fsync(loan_fd) == 0 &&
            append_transaction(original.account_id, 0, "LOAN_CREDIT",
                               loan->amount) == 1) {
            *loan = approved;
            result = 1;
        } else {
            /* Best-effort rollback on I/O failure while both locks are held. */
            (void)pwrite_all(fd, &original, sizeof(original), offset);
            (void)fsync(fd);
            (void)pwrite_all(loan_fd, loan, sizeof(*loan), loan_offset);
            (void)fsync(loan_fd);
        }
        (void)unlock_region(fd, offset, sizeof(AccountRecord));
        break;
    }
    close(fd);
    return result;
}

int process_loan(int loan_id, int employee_id, int approve) {
    if (loan_id <= 0 || employee_id <= 0 || (approve != 0 && approve != 1))
        return -1;
    int fd = open(LOANS_FILE, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -1;

    int result = 0;
    off_t target = -1;
    for (off_t offset = 0;; offset += (off_t)sizeof(LoanRecord)) {
        if (lock_region(fd, F_RDLCK, offset, sizeof(LoanRecord)) < 0) {
            result = -1;
            break;
        }
        LoanRecord loan;
        int rc = pread_all(fd, &loan, sizeof(loan), offset);
        if (unlock_region(fd, offset, sizeof(LoanRecord)) < 0) {
            result = -1;
            break;
        }
        if (rc == 0)
            break;
        if (rc < 0 || !valid_loan(&loan)) {
            result = -1;
            break;
        }
        if (loan.loan_id == loan_id) {
            target = offset;
            break;
        }
    }

    if (result == 0 && target >= 0) {
        if (lock_region(fd, F_WRLCK, target, sizeof(LoanRecord)) < 0) {
            result = -1;
        } else {
            LoanRecord loan;
            if (pread_all(fd, &loan, sizeof(loan), target) != 1 ||
                !valid_loan(&loan) || loan.loan_id != loan_id) {
                result = -1;
            } else if (loan.assigned_employee_id != employee_id) {
                result = -2;
            } else if (loan.status != LOAN_IN_REVIEW) {
                result = -3;
            } else if (approve) {
                result = disburse_loan(fd, target, &loan);
            } else {
                loan.status = LOAN_REJECTED;
                result = pwrite_all(fd, &loan, sizeof(loan), target) == 0 &&
                         fsync(fd) == 0 ? 1 : -1;
            }
            if (unlock_region(fd, target, sizeof(LoanRecord)) < 0 && result == 1)
                result = -1;
        }
    }
    close(fd);
    return result;
}
