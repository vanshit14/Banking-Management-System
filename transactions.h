#ifndef TRANSACTIONS_H
#define TRANSACTIONS_H

#include "common.h"

int append_transaction(int account_id, int other_account_id,
                       const char *type, long long amount);

int list_transactions(int account_id,
                      char rows[][BUFFER_SIZE],
                      int max_rows,
                      int *count);

#endif
