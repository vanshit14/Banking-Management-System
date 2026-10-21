#define _GNU_SOURCE
#include "loans.h"
#include "common.h"
#include "fileio.h"

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
        case LOAN_APPROVED: return "APPROVED";
        case LOAN_REJECTED: return "REJECTED";
        default:            return "UNKNOWN";
    }
}

static LoanStatus parse_status(const char *text) {
    if (strcmp(text, "PENDING") == 0) return LOAN_PENDING;
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
        (void)ftruncate(fd, offset);
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
