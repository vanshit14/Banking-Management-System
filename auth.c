#define _GNU_SOURCE
#include "auth.h"
#include "fileio.h"

#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static sem_t session_sem;
static int active_user_ids[MAX_ACTIVE_SESSIONS];

const char *role_name(Role role) {
    switch (role) {
        case ROLE_CUSTOMER: return "CUSTOMER";
        case ROLE_EMPLOYEE: return "EMPLOYEE";
        case ROLE_MANAGER:  return "MANAGER";
        case ROLE_ADMIN:    return "ADMIN";
        default:            return "NONE";
    }
}

Role parse_role(const char *text) {
    if (strcmp(text, "CUSTOMER") == 0) return ROLE_CUSTOMER;
    if (strcmp(text, "EMPLOYEE") == 0) return ROLE_EMPLOYEE;
    if (strcmp(text, "MANAGER") == 0) return ROLE_MANAGER;
    if (strcmp(text, "ADMIN") == 0) return ROLE_ADMIN;
    return ROLE_NONE;
}

static int valid_user(const UserRecord *user) {
    return user->user_id > 0 && user->username[0] != '\0' &&
           user->password[0] != '\0' &&
           memchr(user->username, '\0', sizeof(user->username)) != NULL &&
           memchr(user->password, '\0', sizeof(user->password)) != NULL &&
           user->role >= ROLE_CUSTOMER && user->role <= ROLE_ADMIN &&
           (user->active == 0 || user->active == 1);
}

static int parse_legacy_user(char *line, UserRecord *user) {
    char *save = NULL;
    char *id_text = strtok_r(line, "|", &save);
    char *username = strtok_r(NULL, "|", &save);
    char *password = strtok_r(NULL, "|", &save);
    char *role_text = strtok_r(NULL, "|", &save);
    char *active_text = strtok_r(NULL, "|", &save);
    char *extra = strtok_r(NULL, "|", &save);
    if (!id_text || !username || !password || !role_text || !active_text || extra ||
        strlen(username) >= sizeof(user->username) ||
        strlen(password) >= sizeof(user->password))
        return 0;

    char *end;
    errno = 0;
    long id = strtol(id_text, &end, 10);
    Role role = parse_role(role_text);
    if (errno || end == id_text || *end != '\0' || id <= 0 ||
        id > 2147483647L || role == ROLE_NONE ||
        (strcmp(active_text, "0") != 0 && strcmp(active_text, "1") != 0))
        return 0;

    memset(user, 0, sizeof(*user));
    user->user_id = (int32_t)id;
    memcpy(user->username, username, strlen(username) + 1);
    memcpy(user->password, password, strlen(password) + 1);
    user->role = role;
    user->active = active_text[0] - '0';
    return valid_user(user);
}

int init_users(void) {
    int fd = open(USERS_FILE, O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
        off_t size = lseek(fd, 0, SEEK_END);
        close(fd);
        return size >= 0 && size % (off_t)sizeof(UserRecord) == 0 ? 0 : -1;
    }
    if (errno != ENOENT)
        return -1;

    int input = open(LEGACY_USERS_FILE, O_RDONLY | O_CLOEXEC);
    if (input < 0)
        return -1;
    int output = open(USERS_TEMP_FILE,
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
        UserRecord user;
        if (!parse_legacy_user(line, &user) ||
            write_all(output, (const char *)&user, sizeof(user)) < 0)
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
    if (rename(USERS_TEMP_FILE, USERS_FILE) < 0 ||
        fsync_path_dir("data") < 0)
        goto done;
    result = 0;

done:
    if (output >= 0)
        close(output);
    close(input);
    if (result != 0)
        unlink(USERS_TEMP_FILE);
    return result;
}

void init_auth(void) {
    if (sem_init(&session_sem, 0, 1) != 0) {
        perror("sem_init");
        exit(EXIT_FAILURE);
    }
}

int authenticate_user(const char *username, const char *password,
                      int *user_id, Role *role) {
    if (!username || !password || !user_id || !role)
        return -1;
    int fd = open(USERS_FILE, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;

    int result = 0;
    for (off_t offset = 0;; offset += (off_t)sizeof(UserRecord)) {
        if (lock_region(fd, F_RDLCK, offset, sizeof(UserRecord)) < 0) {
            result = -1;
            break;
        }
        UserRecord user;
        int rc = pread_all(fd, &user, sizeof(user), offset);
        if (unlock_region(fd, offset, sizeof(UserRecord)) < 0) {
            result = -1;
            break;
        }
        if (rc == 0)
            break;
        if (rc < 0 || !valid_user(&user)) {
            result = -1;
            break;
        }
        if (strcmp(user.username, username) == 0 &&
            strcmp(user.password, password) == 0) {
            if (user.active) {
                *user_id = user.user_id;
                *role = (Role)user.role;
                result = 1;
            }
            break;
        }
    }
    close(fd);
    return result;
}

int change_password(int user_id, const char *old_password,
                    const char *new_password) {
    if (user_id <= 0 || !old_password || !new_password ||
        old_password[0] == '\0' || new_password[0] == '\0' ||
        strlen(new_password) >= sizeof(((UserRecord *)0)->password))
        return -1;
    int fd = open(USERS_FILE, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -1;

    int result = -1;
    for (off_t offset = 0;; offset += (off_t)sizeof(UserRecord)) {
        if (lock_region(fd, F_RDLCK, offset, sizeof(UserRecord)) < 0)
            break;
        UserRecord user;
        int rc = pread_all(fd, &user, sizeof(user), offset);
        if (unlock_region(fd, offset, sizeof(UserRecord)) < 0)
            break;
        if (rc == 0) {
            result = -1;
            break;
        }
        if (rc < 0 || !valid_user(&user))
            break;
        if (user.user_id != user_id)
            continue;

        if (lock_region(fd, F_WRLCK, offset, sizeof(UserRecord)) < 0)
            break;
        rc = pread_all(fd, &user, sizeof(user), offset);
        if (rc != 1 || !valid_user(&user) || user.user_id != user_id) {
            result = -1;
        } else if (strcmp(user.password, old_password) != 0) {
            result = 0;
        } else {
            memset(user.password, 0, sizeof(user.password));
            memcpy(user.password, new_password, strlen(new_password) + 1);
            result = pwrite_all(fd, &user, sizeof(user), offset) == 0 &&
                     fsync(fd) == 0 ? 1 : -1;
        }
        if (unlock_region(fd, offset, sizeof(UserRecord)) < 0 && result == 1)
            result = -1;
        break;
    }
    close(fd);
    return result;
}

static void session_wait(void) {
    while (sem_wait(&session_sem) < 0) {
        if (errno != EINTR) {
            perror("sem_wait");
            exit(EXIT_FAILURE);
        }
    }
}

static void session_post(void) {
    if (sem_post(&session_sem) < 0) {
        perror("sem_post");
        exit(EXIT_FAILURE);
    }
}

int reserve_session(int user_id) {
    int free_slot = -1, result = 0;
    session_wait();
    for (int i = 0; i < MAX_ACTIVE_SESSIONS; i++) {
        if (active_user_ids[i] == user_id) {
            result = -1;
            goto done;
        }
        if (active_user_ids[i] == 0 && free_slot == -1)
            free_slot = i;
    }
    if (free_slot == -1) {
        result = -2;
        goto done;
    }
    active_user_ids[free_slot] = user_id;
    result = 1;
done:
    session_post();
    return result;
}

void release_session(int user_id) {
    if (user_id <= 0)
        return;
    session_wait();
    for (int i = 0; i < MAX_ACTIVE_SESSIONS; i++) {
        if (active_user_ids[i] == user_id) {
            active_user_ids[i] = 0;
            break;
        }
    }
    session_post();
}
