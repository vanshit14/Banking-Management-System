#define _GNU_SOURCE
#include "customer_management.h"
#include "accounts.h"
#include "auth.h"
#include "common.h"
#include "fileio.h"
#include "user_management.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#define MAX_BALANCE 1000000000000000LL

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

static int scan_users(int fd, const char *username, int *next_id,
                      off_t *append_offset) {
    int max_id = 0;
    off_t offset = 0;
    while (1) {
        if (lock_region(fd, F_RDLCK, offset, sizeof(UserRecord)) < 0)
            return -1;
        UserRecord user;
        int rc = pread_all(fd, &user, sizeof(user), offset);
        if (unlock_region(fd, offset, sizeof(UserRecord)) < 0)
            return -1;
        if (rc == 0)
            break;
        if (rc < 0 || !valid_user(&user))
            return -1;
        if (strcmp(user.username, username) == 0)
            return -2;
        if (user.user_id > max_id)
            max_id = user.user_id;
        offset += (off_t)sizeof(UserRecord);
    }
    if (max_id == 2147483647)
        return -1;
    *next_id = max_id + 1;
    *append_offset = offset;
    return 1;
}

static int scan_accounts(int fd, int owner_id, int *next_id,
                         off_t *append_offset) {
    int max_id = 1000;
    off_t offset = 0;
    while (1) {
        if (lock_region(fd, F_RDLCK, offset, sizeof(AccountRecord)) < 0)
            return -1;
        AccountRecord account;
        int rc = pread_all(fd, &account, sizeof(account), offset);
        if (unlock_region(fd, offset, sizeof(AccountRecord)) < 0)
            return -1;
        if (rc == 0)
            break;
        if (rc < 0 || account.account_id <= 0 || account.owner_id <= 0 ||
            account.balance < 0 || account.balance > MAX_BALANCE ||
            (account.active != 0 && account.active != 1) ||
            account.owner_id == owner_id)
            return -1;
        if (account.account_id > max_id)
            max_id = account.account_id;
        offset += (off_t)sizeof(AccountRecord);
    }
    if (max_id == 2147483647)
        return -1;
    *next_id = max_id + 1;
    *append_offset = offset;
    return 1;
}

int add_customer(const char *username, const char *password,
                 long long opening_balance,
                 int *user_id, int *account_id) {
    if (!valid_text_field(username) || !valid_text_field(password) ||
        opening_balance <= 0 || opening_balance > MAX_BALANCE ||
        !user_id || !account_id)
        return -1;

    int users_lock = ofd_lock(USERS_STRUCTURE_LOCK_FILE, F_WRLCK);
    if (users_lock < 0)
        return -1;
    int accounts_lock = ofd_lock(ACCOUNT_STRUCTURE_LOCK_FILE, F_WRLCK);
    if (accounts_lock < 0) {
        ofd_unlock(users_lock);
        return -1;
    }

    int users_fd = open(USERS_FILE, O_RDWR | O_CLOEXEC);
    int accounts_fd = open(ACCOUNTS_FILE, O_RDWR | O_CLOEXEC);
    int result = -1;
    if (users_fd < 0 || accounts_fd < 0)
        goto done;

    off_t user_offset, account_offset;
    result = scan_users(users_fd, username, user_id, &user_offset);
    if (result != 1)
        goto done;
    if (scan_accounts(accounts_fd, *user_id, account_id,
                      &account_offset) != 1) {
        result = -1;
        goto done;
    }

    UserRecord user = {0};
    user.user_id = *user_id;
    memcpy(user.username, username, strlen(username) + 1);
    memcpy(user.password, password, strlen(password) + 1);
    user.role = ROLE_CUSTOMER;
    user.active = 1;
    AccountRecord account = {
        .account_id = *account_id, .owner_id = *user_id,
        .balance = opening_balance, .active = 1, .reserved = 0
    };

    if (lock_region(users_fd, F_WRLCK, user_offset, sizeof(UserRecord)) < 0 ||
        lock_region(accounts_fd, F_WRLCK, account_offset,
                    sizeof(AccountRecord)) < 0) {
        result = -1;
        goto unlock_records;
    }
    if (pwrite_all(accounts_fd, &account, sizeof(account), account_offset) < 0 ||
        fsync(accounts_fd) < 0 ||
        pwrite_all(users_fd, &user, sizeof(user), user_offset) < 0 ||
        fsync(users_fd) < 0) {
        if (ftruncate(accounts_fd, account_offset) < 0) result = -1;
        if (ftruncate(users_fd, user_offset) < 0) result = -1;
        (void)fsync(accounts_fd);
        (void)fsync(users_fd);
        result = -1;
    } else {
        result = 1;
    }

unlock_records:
    (void)unlock_region(accounts_fd, account_offset, sizeof(AccountRecord));
    (void)unlock_region(users_fd, user_offset, sizeof(UserRecord));
done:
    if (accounts_fd >= 0)
        close(accounts_fd);
    if (users_fd >= 0)
        close(users_fd);
    ofd_unlock(accounts_lock);
    ofd_unlock(users_lock);
    return result;
}

int modify_customer_username(int customer_id, const char *new_username) {
    return modify_username_for_role(customer_id, new_username, ROLE_CUSTOMER);
}
