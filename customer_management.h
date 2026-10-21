#ifndef CUSTOMER_MANAGEMENT_H
#define CUSTOMER_MANAGEMENT_H

/*
 * Returns 1 on success, -2 if the username exists, and -1 on storage or
 * validation failure.
 */
int add_customer(const char *username, const char *password,
                 long long opening_balance,
                 int *user_id, int *account_id);

/* Returns 1 on success, 0 if the customer is not found, -2 if taken. */
int modify_customer_username(int customer_id, const char *new_username);

#endif
