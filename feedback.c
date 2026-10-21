#define _GNU_SOURCE
#include "feedback.h"
#include "common.h"
#include "fileio.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int add_feedback(int user_id, const char *text, int *feedback_id) {
    if (user_id <= 0 || !text || text[0] == '\0' ||
        strchr(text, '|') != NULL || strlen(text) >= 400)
        return -1;

    int lock_fd = ofd_lock(FEEDBACK_LOCK_FILE, F_WRLCK);
    if (lock_fd < 0)
        return -1;

    int fd = open(FEEDBACK_FILE, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
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

    *feedback_id = max_id + 1;
    char output[BUFFER_SIZE];
    int n = snprintf(output, sizeof(output),
                     "%d|%d|%ld|%s\n",
                     *feedback_id, user_id, (long)time(NULL), text);
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
