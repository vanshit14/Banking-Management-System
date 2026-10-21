#define _GNU_SOURCE
#include "accounts.h"
#include "common.h"
#include "fileio.h"
#include "transactions.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#define MAX_BALANCE 1000000000000000LL

static int valid_record(const AccountRecord *r) {
    return r->account_id > 0 && r->owner_id > 0 && r->balance >= 0 &&
           r->balance <= MAX_BALANCE && (r->active == 0 || r->active == 1);
}

static int parse_legacy_account(char *line, AccountRecord *r) {
    long long balance = 0;
    char extra;
    int matched = sscanf(line, "%d|%d|%lld.00|%d %c",
                         &r->account_id, &r->owner_id, &balance,
                         &r->active, &extra);
    if (matched != 4)
        matched = sscanf(line, "%d|%d|%lld|%d %c",
                         &r->account_id, &r->owner_id, &balance,
                         &r->active, &extra);
    r->balance = balance;
    r->reserved = 0;
    return matched == 4 && valid_record(r);
}

int init_accounts(void) {
    int fd = open(ACCOUNTS_FILE, O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
        off_t size = lseek(fd, 0, SEEK_END);
        close(fd);
        return size >= 0 && size % (off_t)sizeof(AccountRecord) == 0 ? 0 : -1;
    }
    if (errno != ENOENT)
        return -1;

    int input = open(LEGACY_ACCOUNTS_FILE, O_RDONLY | O_CLOEXEC);
    if (input < 0)
        return -1;
    int output = open(ACCOUNT_TEMP_FILE,
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
        AccountRecord record;
        if (!parse_legacy_account(line, &record) ||
            write_all(output, (const char *)&record, sizeof(record)) < 0)
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
    if (rename(ACCOUNT_TEMP_FILE, ACCOUNTS_FILE) < 0 ||
        fsync_path_dir("data") < 0)
        goto done;
    result = 0;

done:
    if (output >= 0)
        close(output);
    close(input);
    if (result != 0)
        unlink(ACCOUNT_TEMP_FILE);
    return result;
}

static int find_record(int fd, int value, int by_owner, off_t *found_offset) {
    for (off_t offset = 0;; offset += (off_t)sizeof(AccountRecord)) {
        if (lock_region(fd, F_RDLCK, offset, sizeof(AccountRecord)) < 0)
            return -1;
        AccountRecord record;
        int rc = pread_all(fd, &record, sizeof(record), offset);
        if (rc <= 0) {
            unlock_region(fd, offset, sizeof(AccountRecord));
            return rc;
        }
        if (!valid_record(&record)) {
            unlock_region(fd, offset, sizeof(AccountRecord));
            return -1;
        }
        int match = record.active &&
                    (by_owner ? record.owner_id == value
                              : record.account_id == value);
        if (unlock_region(fd, offset, sizeof(AccountRecord)) < 0)
            return -1;
        if (match) {
            *found_offset = offset;
            return 1;
        }
    }
}

int get_account_by_owner(int user_id, int *account_id,
                         long long *balance, int *active) {
    if (user_id <= 0 || !account_id || !balance || !active)
        return -1;
    int fd = open(ACCOUNTS_FILE, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;

    int result = 0;
    for (off_t offset = 0;; offset += (off_t)sizeof(AccountRecord)) {
        if (lock_region(fd, F_RDLCK, offset, sizeof(AccountRecord)) < 0) {
            result = -1;
            break;
        }
        AccountRecord record;
        int rc = pread_all(fd, &record, sizeof(record), offset);
        if (rc == 1 && (!valid_record(&record)))
            rc = -1;
        if (rc == 1 && record.owner_id == user_id) {
            *account_id = record.account_id;
            *balance = record.balance;
            *active = record.active;
            result = 1;
        } else if (rc < 0) {
            result = -1;
        }
        if (unlock_region(fd, offset, sizeof(AccountRecord)) < 0)
            result = -1;
        if (rc == 0 || result != 0)
            break;
    }
    close(fd);
    return result;
}

int set_account_status(int user_id, int active, int *account_id) {
    if (user_id <= 0 || (active != 0 && active != 1) || !account_id)
        return -1;
    int fd = open(ACCOUNTS_FILE, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -1;

    int result = 0;
    off_t target = -1;
    for (off_t offset = 0;; offset += (off_t)sizeof(AccountRecord)) {
        if (lock_region(fd, F_RDLCK, offset, sizeof(AccountRecord)) < 0) {
            result = -1;
            break;
        }
        AccountRecord record;
        int rc = pread_all(fd, &record, sizeof(record), offset);
        if (unlock_region(fd, offset, sizeof(AccountRecord)) < 0) {
            result = -1;
            break;
        }
        if (rc == 0)
            break;
        if (rc < 0 || !valid_record(&record)) {
            result = -1;
            break;
        }
        if (record.owner_id == user_id) {
            target = offset;
            break;
        }
    }

    if (result == 0 && target >= 0) {
        if (lock_region(fd, F_WRLCK, target, sizeof(AccountRecord)) < 0) {
            result = -1;
        } else {
            AccountRecord record;
            if (pread_all(fd, &record, sizeof(record), target) != 1 ||
                !valid_record(&record) || record.owner_id != user_id) {
                result = -1;
            } else {
                record.active = active;
                *account_id = record.account_id;
                result = pwrite_all(fd, &record, sizeof(record), target) == 0 &&
                         fsync(fd) == 0 ? 1 : -1;
            }
            if (unlock_region(fd, target, sizeof(AccountRecord)) < 0 && result == 1)
                result = -1;
        }
    }
    close(fd);
    return result;
}

int get_account_balance(int user_id, int *account_id, long long *balance) {
    int fd = open(ACCOUNTS_FILE, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    off_t offset;
    int result = find_record(fd, user_id, 1, &offset);
    if (result == 1 && lock_region(fd, F_RDLCK, offset,
                                   sizeof(AccountRecord)) < 0)
        result = -1;
    if (result == 1) {
        AccountRecord record;
        if (pread_all(fd, &record, sizeof(record), offset) != 1 ||
            !valid_record(&record) || !record.active ||
            record.owner_id != user_id) {
            result = -1;
        } else {
            *account_id = record.account_id;
            *balance = record.balance;
        }
        if (unlock_region(fd, offset, sizeof(AccountRecord)) < 0)
            result = -1;
    }
    close(fd);
    return result;
}

int update_balance(int user_id, long long amount, int is_deposit,
                   int *account_id, long long *new_balance) {
    if (user_id <= 0 || amount <= 0 || amount > MAX_BALANCE ||
        (is_deposit != 0 && is_deposit != 1))
        return -1;
    int fd = open(ACCOUNTS_FILE, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -1;
    off_t offset;
    int result = find_record(fd, user_id, 1, &offset);
    if (result != 1) {
        close(fd);
        return result;
    }
    if (lock_region(fd, F_WRLCK, offset, sizeof(AccountRecord)) < 0) {
        close(fd);
        return -1;
    }

    AccountRecord record;
    if (pread_all(fd, &record, sizeof(record), offset) != 1 ||
        !valid_record(&record) || !record.active || record.owner_id != user_id) {
        result = -1;
    } else if (!is_deposit && amount > record.balance) {
        result = -2;
    } else if (is_deposit && amount > MAX_BALANCE - record.balance) {
        result = -1;
    } else {
        record.balance += is_deposit ? amount : -amount;
        if (pwrite_all(fd, &record, sizeof(record), offset) < 0 || fsync(fd) < 0) {
            result = -1;
        } else {
            *account_id = record.account_id;
            *new_balance = record.balance;
            result = append_transaction(record.account_id, 0,
                        is_deposit ? "DEPOSIT" : "WITHDRAW", amount) == 1 ? 1 : -5;
        }
    }
    if (unlock_region(fd, offset, sizeof(AccountRecord)) < 0 && result == 1)
        result = -1;
    close(fd);
    return result;
}

int transfer_funds(int user_id, int dest_account_id, long long amount,
                   int *from_account_id, int *to_account_id,
                   long long *from_balance) {
    if (user_id <= 0 || dest_account_id <= 0 || amount <= 0 ||
        amount > MAX_BALANCE)
        return -1;
    int fd = open(ACCOUNTS_FILE, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -1;

    off_t source_offset, dest_offset;
    int result = find_record(fd, user_id, 1, &source_offset);
    if (result != 1)
        goto done;
    result = find_record(fd, dest_account_id, 0, &dest_offset);
    if (result != 1) {
        result = result == 0 ? -3 : -1;
        goto done;
    }
    if (source_offset == dest_offset) {
        result = -4;
        goto done;
    }

    off_t first = source_offset < dest_offset ? source_offset : dest_offset;
    off_t second = source_offset < dest_offset ? dest_offset : source_offset;
    if (lock_region(fd, F_WRLCK, first, sizeof(AccountRecord)) < 0)
        goto storage_error;
    if (lock_region(fd, F_WRLCK, second, sizeof(AccountRecord)) < 0) {
        unlock_region(fd, first, sizeof(AccountRecord));
        goto storage_error;
    }

    AccountRecord source, dest;
    if (pread_all(fd, &source, sizeof(source), source_offset) != 1 ||
        pread_all(fd, &dest, sizeof(dest), dest_offset) != 1 ||
        !valid_record(&source) || !valid_record(&dest) ||
        !source.active || !dest.active || source.owner_id != user_id ||
        dest.account_id != dest_account_id) {
        result = -1;
    } else if (amount > source.balance) {
        result = -2;
    } else if (amount > MAX_BALANCE - dest.balance) {
        result = -1;
    } else {
        AccountRecord old_source = source;
        AccountRecord old_dest = dest;
        source.balance -= amount;
        dest.balance += amount;
        if (pwrite_all(fd, &source, sizeof(source), source_offset) < 0 ||
            pwrite_all(fd, &dest, sizeof(dest), dest_offset) < 0 || fsync(fd) < 0) {
            (void)pwrite_all(fd, &old_source, sizeof(old_source), source_offset);
            (void)pwrite_all(fd, &old_dest, sizeof(old_dest), dest_offset);
            (void)fsync(fd);
            result = -1;
        } else {
            *from_account_id = source.account_id;
            *to_account_id = dest.account_id;
            *from_balance = source.balance;
            result = append_transaction(source.account_id, dest.account_id,
                                        "TRANSFER_OUT", amount) == 1 &&
                     append_transaction(dest.account_id, source.account_id,
                                        "TRANSFER_IN", amount) == 1 ? 1 : -5;
        }
    }
    if (unlock_region(fd, second, sizeof(AccountRecord)) < 0 && result == 1)
        result = -1;
    if (unlock_region(fd, first, sizeof(AccountRecord)) < 0 && result == 1)
        result = -1;
    goto done;

storage_error:
    result = -1;
done:
    close(fd);
    return result;
}
