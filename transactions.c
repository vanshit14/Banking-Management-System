#define _GNU_SOURCE
#include "transactions.h"
#include "fileio.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int append_transaction(int account_id, int other_account_id,
                       const char *type, long long amount) {
    if (account_id <= 0 || other_account_id < 0 || !type ||
        amount <= 0)
        return -1;

    int lock_fd = ofd_lock(TRANSACTIONS_LOCK_FILE, F_WRLCK);
    if (lock_fd < 0)
        return -1;

    int fd = open(TRANSACTIONS_FILE,
                  O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        ofd_unlock(lock_fd);
        return -1;
    }

    int max_id = 0;
    int eof = 0;
    char line[BUFFER_SIZE];

    while (1) {
        int rc = read_line_fd(fd, line, sizeof(line), &eof);
        if (rc == 0)
            break;
        if (rc < 0) {
            close(fd);
            ofd_unlock(lock_fd);
            return -1;
        }

        int txn_id;
        if (sscanf(line, "%d|", &txn_id) == 1 && txn_id > max_id)
            max_id = txn_id;

        if (eof)
            break;
    }

    char output[BUFFER_SIZE];
    int n = snprintf(output, sizeof(output),
                     "%d|%d|%d|%s|%lld|%ld\n",
                     max_id + 1, account_id, other_account_id,
                     type, amount, (long)time(NULL));
    if (n < 0 || (size_t)n >= sizeof(output) ||
        write_all(fd, output, (size_t)n) < 0 ||
        fsync(fd) < 0) {
        close(fd);
        ofd_unlock(lock_fd);
        return -1;
    }

    close(fd);
    ofd_unlock(lock_fd);
    return 1;
}

int list_transactions(int account_id,
                      char rows[][BUFFER_SIZE],
                      int max_rows,
                      int *count) {
    *count = 0;
    if (account_id <= 0 || max_rows <= 0)
        return -1;

    int lock_fd = ofd_lock(TRANSACTIONS_LOCK_FILE, F_RDLCK);
    if (lock_fd < 0)
        return -1;

    int fd = open(TRANSACTIONS_FILE, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        ofd_unlock(lock_fd);
        if (errno == ENOENT)
            return 1;
        return -1;
    }

    int result = 1;
    int eof = 0;
    char line[BUFFER_SIZE];

    while (1) {
        int rc = read_line_fd(fd, line, sizeof(line), &eof);
        if (rc == 0)
            break;
        if (rc < 0) {
            result = -1;
            break;
        }

        int txn_id, acct, other;
        char type[32];
        long long amount;
        long ts;
        char extra;
        if (sscanf(line, "%d|%d|%d|%31[^|]|%lld|%ld %c",
                   &txn_id, &acct, &other, type, &amount, &ts, &extra) != 6) {
            result = -1;
            break;
        }

        if (acct == account_id) {
            if (*count >= max_rows) {
                result = -1;
                break;
            }
            int n = snprintf(rows[*count], BUFFER_SIZE, "%s", line);
            if (n < 0 || (size_t)n >= BUFFER_SIZE) {
                result = -1;
                break;
            }
            (*count)++;
        }

        if (eof)
            break;
    }

    close(fd);
    ofd_unlock(lock_fd);
    return result;
}
