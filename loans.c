#define _GNU_SOURCE
#include "loans.h"
#include "common.h"
#include "fileio.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int apply_loan(int user_id, long long amount, int *loan_id) {
    if (user_id <= 0 || amount <= 0 || amount > 1000000000000000LL)
        return -1;

    int lock_fd = ofd_lock(LOANS_LOCK_FILE, F_WRLCK);
    if (lock_fd < 0)
        return -1;

    int fd = open(LOANS_FILE, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
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

        int id;
        if (sscanf(line, "%d|", &id) == 1 && id > max_id)
            max_id = id;

        if (eof)
            break;
    }

    *loan_id = max_id + 1;
    char output[BUFFER_SIZE];
    int n = snprintf(output, sizeof(output),
                     "%d|%d|%lld|PENDING|0\n",
                     *loan_id, user_id, amount);
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
