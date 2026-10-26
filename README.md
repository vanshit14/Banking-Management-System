# Banking Management System

A Linux client-server banking application written in C. The server maintains
banking records and handles multiple clients using POSIX threads. File
operations and synchronization primarily use Linux system calls, binary files,
and `fcntl` OFD locks.

## Features

### Customer

- Secure login with one active session per user
- View account balance
- Deposit and withdraw integer amounts
- Transfer funds
- Apply for loans
- View transaction history
- Submit feedback
- Change password
- Logout

### Bank Employee

- Add customers
- Modify customer details
- Permanently close customer accounts
- View customer transaction history
- View assigned loan applications
- Start loan processing
- Approve or reject loans
- Change password

### Manager

- Activate or deactivate customer accounts
- Assign loans to employees
- Review customer feedback
- Change password

### Administrator

- Add bank employees
- Modify customer and employee details
- Change employee and manager roles
- Change password

## Technical Features

- TCP socket communication
- Multithreaded server
- One thread per connected client
- Binary record storage
- `fcntl` OFD file and record locking
- Ordered locking during transfers to prevent deadlocks
- One active login session per user
- Role-based authorization
- Integer-only monetary values
- Modular C source files
- `fsync()` for important file updates

## Compilation

Open a terminal inside the project folder:

```bash
cd ~/banking-review/banking-system
```

Compile the server:

```bash
gcc -Wall -Wextra -Wpedantic -pthread \
server.c auth.c accounts.c customer_management.c user_management.c \
fileio.c transactions.c loans.c feedback.c -o server
```

Compile the client:

```bash
gcc -Wall -Wextra -Wpedantic client.c -o client
```

## Running the Project

Start the server first:

```bash
./server
```

Open another terminal in the same folder and start a client:

```bash
./client
```

Additional clients can be started in separate terminals using the same command.
The server listens on `127.0.0.1:5555`.

The initial text files under `data/` are converted automatically into binary
record files when the server starts for the first time.

## Design

See [CLASS_DIAGRAM.md](CLASS_DIAGRAM.md) for the UML-style representation of
the project structures, modules, operations, and relationships.
