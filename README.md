# Banking Management System

Build on Linux with:

```sh
gcc -Wall -Wextra -Wpedantic -pthread server.c auth.c accounts.c \
    customer_management.c user_management.c fileio.c transactions.c \
    loans.c feedback.c -o server
gcc -Wall -Wextra -Wpedantic client.c -o client
```

Then start `./server` and `./client` in separate terminals from this directory.
The server listens on `127.0.0.1:5555`. Existing text records in `data/` are
migrated automatically on first startup. Password storage remains unchanged.

## Concurrency and consistency

The server creates one thread for each connected client. Binary records are
protected with `fcntl` OFD record locks, so operations on different customer
records can run concurrently while operations on the same record wait. A
transfer locks both account records in file-offset order to prevent deadlocks.
Related file updates use `fsync`, and multi-record operations perform a local
best-effort rollback when an error is detected.

## Account closing

Employees select option 11 to permanently close a customer account and confirm
with `CLOSE`. Closure requires a zero balance and no pending or in-review loans.
It preserves records and passbook history, blocks banking operations and login,
and cannot be reversed by manager activation. Manager deactivation remains
temporary. A new customer/account can be created with a different username.

