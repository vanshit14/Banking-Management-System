#define _GNU_SOURCE
#include "fileio.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

int ofd_lock(const char *lock_path, int lock_type) {
    int fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;

    struct flock lock = {0};
    lock.l_type = (short)lock_type;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;

    while (fcntl(fd, F_OFD_SETLKW, &lock) < 0) {
        if (errno == EINTR)
            continue;
        close(fd);
        return -1;
    }
    return fd;
}

void ofd_unlock(int lock_fd) {
    if (lock_fd < 0)
        return;

    struct flock lock = {0};
    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;
    while (fcntl(lock_fd, F_OFD_SETLK, &lock) < 0 && errno == EINTR) {}
    close(lock_fd);
}

int lock_region(int fd, int lock_type, off_t start, off_t length) {
    struct flock lock = {0};
    lock.l_type = (short)lock_type;
    lock.l_whence = SEEK_SET;
    lock.l_start = start;
    lock.l_len = length;
    while (fcntl(fd, F_OFD_SETLKW, &lock) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return 0;
}

int unlock_region(int fd, off_t start, off_t length) {
    struct flock lock = {0};
    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = start;
    lock.l_len = length;
    while (fcntl(fd, F_OFD_SETLK, &lock) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return 0;
}

int read_line_fd(int fd, char *buf, size_t cap, int *eof) {
    size_t used = 0;
    *eof = 0;

    while (used < cap - 1) {
        char ch;
        ssize_t n = read(fd, &ch, 1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) {
            *eof = 1;
            if (used == 0)
                return 0;
            break;
        }
        if (ch == '\n')
            break;
        if (ch == '\0')
            return -1;
        buf[used++] = ch;
    }

    if (used == cap - 1) {
        char ch;
        ssize_t n;
        do {
            n = read(fd, &ch, 1);
        } while (n < 0 && errno == EINTR);
        if (n > 0 && ch != '\n')
            return -1;
        if (n < 0)
            return -1;
        if (n == 0)
            *eof = 1;
    }

    buf[used] = '\0';
    return 1;
}

int write_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) {
            errno = EIO;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

int pread_all(int fd, void *buf, size_t len, off_t offset) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = pread(fd, (char *)buf + done, len - done,
                          offset + (off_t)done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return done == 0 ? 0 : -1;
        done += (size_t)n;
    }
    return 1;
}

int pwrite_all(int fd, const void *buf, size_t len, off_t offset) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = pwrite(fd, (const char *)buf + done, len - done,
                           offset + (off_t)done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) {
            errno = EIO;
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

int fsync_path_dir(const char *dir_path) {
    int dir_fd = open(dir_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd < 0)
        return -1;
    if (fsync(dir_fd) < 0) {
        close(dir_fd);
        return -1;
    }
    close(dir_fd);
    return 0;
}
