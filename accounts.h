#ifndef ACCOUNTS_H
#define ACCOUNTS_H

#include <stdint.h>

typedef struct {
    int32_t account_id;
    int32_t owner_id;
    int64_t balance;
    int32_t active;
    int32_t reserved; /* 1 means permanently closed; 0 means open. */
} AccountRecord;

int init_accounts(void);
/* Closing requires zero balance and no pending/in-review loans.
 * Returns 1, 0 (missing), -2 (balance), -3 (loans), -4 (closed), -1 (I/O). */
int close_account(int user_id);
int account_is_closed(int user_id);
int get_account_balance(int user_id,
                        int *account_id,
                        long long *balance);

int get_account_by_owner(int user_id, int *account_id,
                         long long *balance, int *active);

int set_account_status(int user_id, int active, int *account_id);

int update_balance(int user_id, long long amount,
                   int is_deposit, int *account_id,
                   long long *new_balance);

int transfer_funds(int user_id, int dest_account_id, long long amount,
                   int *from_account_id, int *to_account_id,
                   long long *from_balance);

#endif
