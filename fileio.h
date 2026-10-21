#ifndef FILEIO_H
#define FILEIO_H

#include <stddef.h>
#include <sys/types.h>

int ofd_lock(const char *lock_path, int lock_type);
void ofd_unlock(int lock_fd);
int lock_region(int fd, int lock_type, off_t start, off_t length);
int unlock_region(int fd, off_t start, off_t length);
int read_line_fd(int fd, char *buf, size_t cap, int *eof);
int write_all(int fd, const char *buf, size_t len);
int pread_all(int fd, void *buf, size_t len, off_t offset);
int pwrite_all(int fd, const void *buf, size_t len, off_t offset);
int fsync_path_dir(const char *dir_path);

#endif
