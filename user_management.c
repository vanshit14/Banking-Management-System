#define _GNU_SOURCE
#include "user_management.h"
#include "auth.h"
#include "common.h"
#include "fileio.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static int valid_text_field(const char *text) {
    return text && text[0] != '\0' && strlen(text) < 128 &&
           strchr(text, '|') == NULL && strchr(text, '\n') == NULL &&
           strchr(text, '\r') == NULL;
}

static int valid_user(const UserRecord *user) {
    return user->user_id > 0 && user->username[0] != '\0' &&
           user->password[0] != '\0' &&
           memchr(user->username, '\0', sizeof(user->username)) != NULL &&
           memchr(user->password, '\0', sizeof(user->password)) != NULL &&
           user->role >= ROLE_CUSTOMER && user->role <= ROLE_ADMIN &&
           (user->active == 0 || user->active == 1);
}

int add_employee(const char *username, const char *password, int *user_id) {
    if (!valid_text_field(username) || !valid_text_field(password) || !user_id)
        return -1;
    int structure_lock = ofd_lock(USERS_STRUCTURE_LOCK_FILE, F_WRLCK);
    if (structure_lock < 0)
        return -1;
    int fd = open(USERS_FILE, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        ofd_unlock(structure_lock);
        return -1;
    }

    int max_id = 0, result = -1;
    off_t offset = 0;
    while (1) {
        if (lock_region(fd, F_RDLCK, offset, sizeof(UserRecord)) < 0)
            goto done;
        UserRecord user;
        int rc = pread_all(fd, &user, sizeof(user), offset);
        if (unlock_region(fd, offset, sizeof(UserRecord)) < 0)
            goto done;
        if (rc == 0)
            break;
        if (rc < 0 || !valid_user(&user))
            goto done;
        if (strcmp(user.username, username) == 0) {
            result = -2;
            goto done;
        }
        if (user.user_id > max_id)
            max_id = user.user_id;
        offset += (off_t)sizeof(UserRecord);
    }
    if (max_id == 2147483647)
        goto done;

    UserRecord employee = {0};
    employee.user_id = max_id + 1;
    memcpy(employee.username, username, strlen(username) + 1);
    memcpy(employee.password, password, strlen(password) + 1);
    employee.role = ROLE_EMPLOYEE;
    employee.active = 1;

    if (lock_region(fd, F_WRLCK, offset, sizeof(UserRecord)) < 0)
        goto done;
    if (pwrite_all(fd, &employee, sizeof(employee), offset) == 0 &&
        fsync(fd) == 0) {
        *user_id = employee.user_id;
        result = 1;
    } else {
        if (ftruncate(fd, offset) < 0) result = -1;
        (void)fsync(fd);
    }
    (void)unlock_region(fd, offset, sizeof(UserRecord));

done:
    close(fd);
    ofd_unlock(structure_lock);
    return result;
}

static int role_can_be_modified(Role actual, Role required) {
    if (required == ROLE_NONE)
        return actual == ROLE_CUSTOMER || actual == ROLE_EMPLOYEE ||
               actual == ROLE_MANAGER;
    return actual == required;
}

int modify_username_for_role(int user_id, const char *new_username,
                             Role required_role) {
    if (user_id <= 0 || !valid_text_field(new_username) ||
        (required_role != ROLE_NONE && required_role != ROLE_CUSTOMER &&
         required_role != ROLE_EMPLOYEE))
        return -1;
    int structure_lock = ofd_lock(USERS_STRUCTURE_LOCK_FILE, F_WRLCK);
    if (structure_lock < 0)
        return -1;
    int fd = open(USERS_FILE, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        ofd_unlock(structure_lock);
        return -1;
    }

    int result = 0;
    off_t target = -1;
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
        if (user.user_id != user_id &&
            strcmp(user.username, new_username) == 0) {
            result = -2;
            break;
        }
        if (user.user_id == user_id) {
            if (target >= 0) {
                result = -1;
                break;
            }
            if (!role_can_be_modified((Role)user.role, required_role)) {
                result = 0;
                break;
            }
            target = offset;
        }
    }

    if (result == 0 && target >= 0) {
        if (lock_region(fd, F_WRLCK, target, sizeof(UserRecord)) < 0) {
            result = -1;
        } else {
            UserRecord user;
            if (pread_all(fd, &user, sizeof(user), target) != 1 ||
                !valid_user(&user) || user.user_id != user_id ||
                !role_can_be_modified((Role)user.role, required_role)) {
                result = -1;
            } else {
                memset(user.username, 0, sizeof(user.username));
                memcpy(user.username, new_username, strlen(new_username) + 1);
                result = pwrite_all(fd, &user, sizeof(user), target) == 0 &&
                         fsync(fd) == 0 ? 1 : -1;
            }
            if (unlock_region(fd, target, sizeof(UserRecord)) < 0 && result == 1)
                result = -1;
        }
    }
    close(fd);
    ofd_unlock(structure_lock);
    return result;
}

int admin_modify_username(int user_id, const char *new_username) {
    return modify_username_for_role(user_id, new_username, ROLE_NONE);
}

int set_staff_role(int user_id, Role new_role) {
    if (user_id <= 0 ||
        (new_role != ROLE_EMPLOYEE && new_role != ROLE_MANAGER))
        return -1;
    int structure_lock = ofd_lock(USERS_STRUCTURE_LOCK_FILE, F_WRLCK);
    if (structure_lock < 0)
        return -1;
    int fd = open(USERS_FILE, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        ofd_unlock(structure_lock);
        return -1;
    }

    int result = 0;
    off_t target = -1;
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
        if (user.user_id == user_id) {
            if ((user.role != ROLE_EMPLOYEE && user.role != ROLE_MANAGER) ||
                target >= 0) {
                result = user.role == ROLE_EMPLOYEE || user.role == ROLE_MANAGER
                         ? -1 : -2;
                break;
            }
            target = offset;
        }
    }

    if (result == 0 && target >= 0) {
        if (lock_region(fd, F_WRLCK, target, sizeof(UserRecord)) < 0) {
            result = -1;
        } else {
            UserRecord user;
            if (pread_all(fd, &user, sizeof(user), target) != 1 ||
                !valid_user(&user) || user.user_id != user_id ||
                (user.role != ROLE_EMPLOYEE && user.role != ROLE_MANAGER)) {
                result = -1;
            } else {
                user.role = new_role;
                result = pwrite_all(fd, &user, sizeof(user), target) == 0 &&
                         fsync(fd) == 0 ? 1 : -1;
            }
            if (unlock_region(fd, target, sizeof(UserRecord)) < 0 && result == 1)
                result = -1;
        }
    }
    close(fd);
    ofd_unlock(structure_lock);
    return result;
}
