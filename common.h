#ifndef COMMON_H
#define COMMON_H

#define PORT 5555
#define BUFFER_SIZE 1024
#define MAX_ACTIVE_SESSIONS 128
#define MAX_HISTORY_ROWS 256
#define MAX_LOAN_ROWS 256
#define MAX_FEEDBACK_ROWS 256

#define USERS_FILE "data/users.dat"
#define LEGACY_USERS_FILE "data/users.txt"
#define USERS_TEMP_FILE "data/users.dat.tmp"
#define USERS_STRUCTURE_LOCK_FILE "data/users-structure.lock"

#define ACCOUNTS_FILE "data/accounts.dat"
#define LEGACY_ACCOUNTS_FILE "data/accounts.txt"
#define ACCOUNT_TEMP_FILE "data/accounts.dat.tmp"
#define ACCOUNT_STRUCTURE_LOCK_FILE "data/accounts-structure.lock"

#define TRANSACTIONS_FILE "data/transactions.txt"
#define TRANSACTIONS_LOCK_FILE "data/transactions.lock"

#define LOANS_FILE "data/loans.dat"
#define LEGACY_LOANS_FILE "data/loans.txt"
#define LOANS_TEMP_FILE "data/loans.dat.tmp"
#define LOANS_STRUCTURE_LOCK_FILE "data/loans-structure.lock"

#define FEEDBACK_FILE "data/feedback.txt"
#define FEEDBACK_LOCK_FILE "data/feedback.lock"

typedef enum {
    ROLE_NONE = 0,
    ROLE_CUSTOMER,
    ROLE_EMPLOYEE,
    ROLE_MANAGER,
    ROLE_ADMIN
} Role;

typedef struct {
    int socket_fd;
    int user_id;          /* 0 means not authenticated */
    Role role;
    int authenticated;
} ClientSession;

#endif
