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

int list_feedback(char rows[][BUFFER_SIZE], int max_rows, int *count) {
    if (!rows || max_rows <= 0 || !count)
        return -1;
    *count = 0;
    int lock_fd = ofd_lock(FEEDBACK_LOCK_FILE, F_RDLCK);
    if (lock_fd < 0)
        return -1;
    int fd = open(FEEDBACK_FILE, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        int missing = errno == ENOENT;
        ofd_unlock(lock_fd);
        return missing ? 1 : -1;
    }

    int result = 1, eof = 0;
    char line[BUFFER_SIZE];
    while (1) {
        int rc = read_line_fd(fd, line, sizeof(line), &eof);
        if (rc == 0)
            break;
        if (rc < 0 || *count >= max_rows) {
            result = -1;
            break;
        }
        int feedback_id, user_id;
        long timestamp;
        char text[400];
        if (sscanf(line, "%d|%d|%ld|%399[^\n]",
                   &feedback_id, &user_id, &timestamp, text) != 4 ||
            feedback_id <= 0 || user_id <= 0 || timestamp <= 0 ||
            text[0] == '\0' || strchr(text, '|') != NULL) {
            result = -1;
            break;
        }
        int length = snprintf(rows[*count], BUFFER_SIZE, "%s", line);
        if (length < 0 || (size_t)length >= BUFFER_SIZE) {
            result = -1;
            break;
        }
        (*count)++;
        if (eof)
            break;
    }
    close(fd);
    ofd_unlock(lock_fd);
    return result;
}
