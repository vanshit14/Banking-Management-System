#ifndef LOANS_H
#define LOANS_H

#include <stdint.h>

typedef enum {
    LOAN_PENDING = 1,
    LOAN_APPROVED,
    LOAN_REJECTED
} LoanStatus;

typedef struct {
    int32_t loan_id;
    int32_t customer_id;
    int64_t amount;
    int32_t status;
    int32_t assigned_employee_id;
} LoanRecord;

int init_loans(void);
int apply_loan(int user_id, long long amount, int *loan_id);
int assign_loan(int loan_id, int employee_id);

#endif
