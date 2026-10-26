# Banking Management System — Class Diagram

The project is implemented in C, so the boxes below represent C structures,
enumerations, and modules rather than C++ or Java classes. Public module
functions are shown as operations.

```mermaid
classDiagram
    direction LR

    class Role {
        <<enumeration>>
        CUSTOMER
        EMPLOYEE
        MANAGER
        ADMIN
    }

    class UserRecord {
        +int user_id
        +char username[128]
        +char password[128]
        +Role role
        +int active
    }

    class ClientSession {
        +int socket_fd
        +int user_id
        +Role role
        +int authenticated
    }

    class AccountRecord {
        +int account_id
        +int owner_id
        +long long balance
        +int active
        +int reserved
    }

    class LoanStatus {
        <<enumeration>>
        PENDING
        IN_REVIEW
        APPROVED
        REJECTED
    }

    class LoanRecord {
        +int loan_id
        +int customer_id
        +long long amount
        +LoanStatus status
        +int assigned_employee_id
    }

    class Authentication {
        <<module>>
        +authenticate_user(username, password)
        +change_password(user_id, old, new)
        +reserve_session(user_id)
        +release_session(user_id)
    }

    class AccountManagement {
        <<module>>
        +get_account_balance(user_id)
        +update_balance(user_id, amount, type)
        +transfer_funds(user_id, destination, amount)
        +set_account_status(user_id, active)
        +close_account(user_id)
    }

    class CustomerManagement {
        <<module>>
        +add_customer(username, password, balance)
        +modify_customer_username(user_id, username)
    }

    class UserManagement {
        <<module>>
        +add_employee(username, password)
        +admin_modify_username(user_id, username)
        +set_staff_role(user_id, role)
    }

    class LoanManagement {
        <<module>>
        +apply_loan(customer_id, amount)
        +assign_loan(loan_id, employee_id)
        +start_loan_processing(loan_id, employee_id)
        +process_loan(loan_id, employee_id, decision)
        +list_assigned_loans(employee_id)
    }

    class TransactionHistory {
        <<module>>
        +append_transaction(account_id, other_id, type, amount)
        +list_transactions(account_id)
    }

    class FeedbackManagement {
        <<module>>
        +add_feedback(customer_id, text)
        +list_feedback()
    }

    class FileIO {
        <<module>>
        +lock_region(fd, type, offset, length)
        +unlock_region(fd, offset, length)
        +pread_all(fd, buffer, length, offset)
        +pwrite_all(fd, buffer, length, offset)
        +ofd_lock(path, type)
    }

    Role <-- UserRecord : identifies role
    Role <-- ClientSession : authorizes requests
    UserRecord "1" --> "0..1" ClientSession : authenticates into
    UserRecord "1" --> "0..1" AccountRecord : customer owns
    UserRecord "1" --> "0..*" LoanRecord : customer applies
    UserRecord "0..1" --> "0..*" LoanRecord : employee processes
    LoanStatus <-- LoanRecord : has state

    Authentication ..> UserRecord : reads and updates
    Authentication ..> ClientSession : manages
    CustomerManagement ..> UserRecord : creates and modifies
    CustomerManagement ..> AccountRecord : creates
    UserManagement ..> UserRecord : manages staff
    AccountManagement ..> AccountRecord : reads and updates
    AccountManagement ..> TransactionHistory : records operations
    LoanManagement ..> LoanRecord : manages
    LoanManagement ..> AccountRecord : credits approved loan
    LoanManagement ..> TransactionHistory : records loan credit
    FeedbackManagement ..> UserRecord : submitted by customer

    Authentication ..> FileIO : uses locks and file I/O
    CustomerManagement ..> FileIO : uses locks and file I/O
    UserManagement ..> FileIO : uses locks and file I/O
    AccountManagement ..> FileIO : uses record locks
    LoanManagement ..> FileIO : uses record locks
    TransactionHistory ..> FileIO : uses file locks
    FeedbackManagement ..> FileIO : uses file locks
```

## Role responsibilities

- **Customer:** balance, deposit, withdrawal, transfer, loan application,
  password change, feedback, and transaction history.
- **Employee:** customer creation and modification, account closing, customer
  history, and assigned-loan processing.
- **Manager:** account activation or deactivation, loan assignment, and
  feedback review.
- **Administrator:** employee creation, user modification, and staff-role
  management.

The server creates a `ClientSession` for every connected client. It checks the
session role before calling the appropriate module. Storage modules use
`FileIO` helpers and `fcntl` OFD locks to synchronize access to binary records.
