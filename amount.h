#ifndef AMOUNT_H
#define AMOUNT_H

#include <errno.h>
#include <stdlib.h>
#include <limits.h>

/* Whole positive amounts only. */
static inline int parse_money_amount(const char *text, long long *amount) {
    if (!text || !amount || *text < '0' || *text > '9')
        return 0;

    const char *p = text;
    while (*p >= '0' && *p <= '9')
        p++;
    if (*p != '\0')
        return 0;

    char *end;
    errno = 0;
    long long value = strtoll(text, &end, 10);
    if (errno || *end != '\0' || value <= 0 || value > 1000000000000000LL)
        return 0;

    *amount = value;
    return 1;
}

#endif
