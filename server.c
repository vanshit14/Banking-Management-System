#include "common.h"
#include "amount.h"
#include "accounts.h"
#include "auth.h"
#include "customer_management.h"
#include "feedback.h"
#include "loans.h"
#include "transactions.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int send_all(int fd, const char *data, size_t length) {
    size_t sent = 0;

    while (sent < length) {
        ssize_t n = send(fd, data + sent, length - sent, 0);

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (n == 0) {
            errno = EPIPE;
            return -1;
        }

        sent += (size_t)n;
    }

    return 1;
}

static int read_line(int fd, char *buffer, size_t capacity) {
    size_t used = 0;

    if (capacity < 2)
        return -1;

    while (used < capacity - 1) {
        char ch;
        ssize_t n = recv(fd, &ch, 1, 0);

        if (n == 0) {
            if (used == 0)
                return 0;
            buffer[used] = '\0';
            return 1;
        }

        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (ch == '\n') {
            buffer[used] = '\0';
            return 1;
        }

        if (ch == '\0' || ch == '\r')
            return -1;

        buffer[used++] = ch;
    }

    return -1;
}

static int reply(int fd, const char *message) {
    char output[BUFFER_SIZE + 16];
    int n = snprintf(output, sizeof(output), "%s\n", message);

    if (n < 0 || (size_t)n >= sizeof(output))
        return -1;

    return send_all(fd, output, (size_t)n);
}

static int require_customer(ClientSession *session) {
    if (!session->authenticated) {
        reply(session->socket_fd, "ERR|LOGIN_REQUIRED");
        return 0;
    }
    if (session->role != ROLE_CUSTOMER) {
        reply(session->socket_fd, "ERR|CUSTOMER_ONLY");
        return 0;
    }
    return 1;
}

static int require_employee(ClientSession *session) {
    if (!session->authenticated) {
        reply(session->socket_fd, "ERR|LOGIN_REQUIRED");
        return 0;
    }
    if (session->role != ROLE_EMPLOYEE) {
        reply(session->socket_fd, "ERR|EMPLOYEE_ONLY");
        return 0;
    }
    return 1;
}

static int require_manager(ClientSession *session) {
    if (!session->authenticated) {
        reply(session->socket_fd, "ERR|LOGIN_REQUIRED");
        return 0;
    }
    if (session->role != ROLE_MANAGER) {
        reply(session->socket_fd, "ERR|MANAGER_ONLY");
        return 0;
    }
    return 1;
}

static int parse_positive_amount(const char *text, long long *amount) {
    return parse_money_amount(text, amount);
}

static void reply_money_result(int fd, int result, const char *ok_fmt,
                               int account_id, long long balance) {
    if (result == 1) {
        char response[BUFFER_SIZE];
        snprintf(response, sizeof(response), ok_fmt, account_id, balance);
        reply(fd, response);
    } else if (result == 0) {
        reply(fd, "ERR|NO_ACTIVE_ACCOUNT");
    } else if (result == -2) {
        reply(fd, "ERR|INSUFFICIENT_FUNDS");
    } else if (result == -5) {
        reply(fd, "ERR|TXN_LOG_FAILED");
    } else {
        reply(fd, "ERR|ACCOUNT_UPDATE_FAILED");
    }
}

static void send_transaction_history(int fd, int account_id) {
    char rows[MAX_HISTORY_ROWS][BUFFER_SIZE];
    int count = 0;
    if (list_transactions(account_id, rows, MAX_HISTORY_ROWS, &count) != 1) {
        reply(fd, "ERR|HISTORY_FAILED");
        return;
    }

    char header[BUFFER_SIZE];
    snprintf(header, sizeof(header), "OK|HISTORY|%d", count);
    reply(fd, header);
    for (int i = 0; i < count; i++) {
        char row[BUFFER_SIZE + 8];
        snprintf(row, sizeof(row), "ROW|%s", rows[i]);
        reply(fd, row);
    }
    reply(fd, "END");
}

static void *handle_client(void *arg) {
    ClientSession *session = arg;
    char buffer[BUFFER_SIZE];

    printf("Client connected (fd=%d)\n", session->socket_fd);

    while (1) {
        int rc = read_line(session->socket_fd, buffer, sizeof(buffer));

        if (rc <= 0)
            break;

        char request[BUFFER_SIZE];
        memcpy(request, buffer, strlen(buffer) + 1);

        char *save = NULL;
        char *command = strtok_r(request, "|", &save);

        if (command == NULL) {
            reply(session->socket_fd, "ERR|EMPTY_REQUEST");
            continue;
        }

        if (strcmp(command, "LOGIN") == 0) {
            char *username = strtok_r(NULL, "|", &save);
            char *password = strtok_r(NULL, "|", &save);
            char *extra = strtok_r(NULL, "|", &save);

            if (!username || !password || extra ||
                username[0] == '\0' || password[0] == '\0' ||
                strlen(username) >= 128 ||
                strlen(password) >= 128) {
                reply(session->socket_fd, "ERR|INVALID_LOGIN_FORMAT");
                continue;
            }

            if (session->authenticated) {
                reply(session->socket_fd, "ERR|ALREADY_AUTHENTICATED");
                continue;
            }

            int user_id = 0;
            Role role = ROLE_NONE;
            int auth = authenticate_user(username, password, &user_id, &role);

            if (auth < 0) {
                reply(session->socket_fd, "ERR|USER_STORAGE_ERROR");
                continue;
            }
            if (auth == 0) {
                reply(session->socket_fd, "ERR|INVALID_CREDENTIALS_OR_INACTIVE");
                continue;
            }

            int reserved = reserve_session(user_id);
            if (reserved == -1) {
                reply(session->socket_fd, "ERR|USER_ALREADY_LOGGED_IN");
                continue;
            }
            if (reserved == -2) {
                reply(session->socket_fd, "ERR|SERVER_SESSION_LIMIT");
                continue;
            }

            session->user_id = user_id;
            session->role = role;
            session->authenticated = 1;

            char response[BUFFER_SIZE];
            snprintf(response, sizeof(response),
                     "OK|LOGIN|%d|%s", user_id, role_name(role));
            reply(session->socket_fd, response);
        } else if (strcmp(command, "ADD_CUSTOMER") == 0) {
            if (!require_employee(session))
                continue;

            char *username = strtok_r(NULL, "|", &save);
            char *password = strtok_r(NULL, "|", &save);
            char *balance_text = strtok_r(NULL, "|", &save);
            char *extra = strtok_r(NULL, "|", &save);
            long long opening_balance;
            if (!username || !password || !balance_text || extra ||
                username[0] == '\0' || password[0] == '\0' ||
                strlen(username) >= 128 || strlen(password) >= 128 ||
                !parse_positive_amount(balance_text, &opening_balance)) {
                reply(session->socket_fd, "ERR|INVALID_CUSTOMER_DATA");
                continue;
            }

            int user_id = 0, account_id = 0;
            int result = add_customer(username, password, opening_balance,
                                      &user_id, &account_id);
            if (result == -2) {
                reply(session->socket_fd, "ERR|USERNAME_EXISTS");
            } else if (result != 1) {
                reply(session->socket_fd, "ERR|CUSTOMER_CREATE_FAILED");
            } else {
                char response[BUFFER_SIZE];
                snprintf(response, sizeof(response),
                         "OK|CUSTOMER_ADDED|%d|%d", user_id, account_id);
                reply(session->socket_fd, response);
            }
        } else if (strcmp(command, "MODIFY_CUSTOMER") == 0) {
            if (!require_employee(session))
                continue;

            char *id_text = strtok_r(NULL, "|", &save);
            char *new_username = strtok_r(NULL, "|", &save);
            char *extra = strtok_r(NULL, "|", &save);
            char *end = NULL;
            errno = 0;
            long customer_id = id_text ? strtol(id_text, &end, 10) : 0;
            if (!id_text || !new_username || extra || errno ||
                end == id_text || *end != '\0' || customer_id <= 0 ||
                customer_id > 2147483647L || new_username[0] == '\0' ||
                strlen(new_username) >= 128) {
                reply(session->socket_fd, "ERR|INVALID_CUSTOMER_DATA");
                continue;
            }

            int result = modify_customer_username((int)customer_id,
                                                  new_username);
            if (result == 1)
                reply(session->socket_fd, "OK|CUSTOMER_MODIFIED");
            else if (result == 0)
                reply(session->socket_fd, "ERR|CUSTOMER_NOT_FOUND");
            else if (result == -2)
                reply(session->socket_fd, "ERR|USERNAME_EXISTS");
            else
                reply(session->socket_fd, "ERR|CUSTOMER_MODIFY_FAILED");
        } else if (strcmp(command, "CUSTOMER_HISTORY") == 0) {
            if (!require_employee(session))
                continue;

            char *id_text = strtok_r(NULL, "|", &save);
            char *extra = strtok_r(NULL, "|", &save);
            char *end = NULL;
            errno = 0;
            long customer_id = id_text ? strtol(id_text, &end, 10) : 0;
            if (!id_text || extra || errno || end == id_text || *end != '\0' ||
                customer_id <= 0 || customer_id > 2147483647L) {
                reply(session->socket_fd, "ERR|INVALID_CUSTOMER_ID");
                continue;
            }

            int account_id, active;
            long long balance;
            int found = get_account_by_owner((int)customer_id, &account_id,
                                             &balance, &active);
            if (found < 0)
                reply(session->socket_fd, "ERR|ACCOUNT_STORAGE_ERROR");
            else if (found == 0)
                reply(session->socket_fd, "ERR|CUSTOMER_ACCOUNT_NOT_FOUND");
            else
                send_transaction_history(session->socket_fd, account_id);
        } else if (strcmp(command, "ASSIGN_LOAN") == 0) {
            if (!require_manager(session))
                continue;

            char *loan_text = strtok_r(NULL, "|", &save);
            char *employee_text = strtok_r(NULL, "|", &save);
            char *extra = strtok_r(NULL, "|", &save);
            char *loan_end = NULL, *employee_end = NULL;
            errno = 0;
            long loan_id = loan_text ? strtol(loan_text, &loan_end, 10) : 0;
            long employee_id = employee_text ?
                               strtol(employee_text, &employee_end, 10) : 0;
            if (!loan_text || !employee_text || extra || errno ||
                loan_end == loan_text || *loan_end != '\0' ||
                employee_end == employee_text || *employee_end != '\0' ||
                loan_id <= 0 || loan_id > 2147483647L ||
                employee_id <= 0 || employee_id > 2147483647L) {
                reply(session->socket_fd, "ERR|INVALID_LOAN_ASSIGNMENT");
                continue;
            }

            Role employee_role;
            int employee_active;
            int employee_found = get_user_role((int)employee_id,
                                               &employee_role,
                                               &employee_active);
            if (employee_found < 0) {
                reply(session->socket_fd, "ERR|USER_STORAGE_ERROR");
                continue;
            }
            if (employee_found == 0 || employee_role != ROLE_EMPLOYEE ||
                !employee_active) {
                reply(session->socket_fd, "ERR|EMPLOYEE_NOT_FOUND");
                continue;
            }

            int result = assign_loan((int)loan_id, (int)employee_id);
            if (result == 1)
                reply(session->socket_fd, "OK|LOAN_ASSIGNED");
            else if (result == 0)
                reply(session->socket_fd, "ERR|LOAN_NOT_FOUND");
            else if (result == -2)
                reply(session->socket_fd, "ERR|LOAN_NOT_ASSIGNABLE");
            else
                reply(session->socket_fd, "ERR|LOAN_ASSIGNMENT_FAILED");
        } else if (strcmp(command, "SET_ACCOUNT_STATUS") == 0) {
            if (!require_manager(session))
                continue;

            char *id_text = strtok_r(NULL, "|", &save);
            char *status_text = strtok_r(NULL, "|", &save);
            char *extra = strtok_r(NULL, "|", &save);
            char *end = NULL;
            errno = 0;
            long customer_id = id_text ? strtol(id_text, &end, 10) : 0;
            if (!id_text || !status_text || extra || errno ||
                end == id_text || *end != '\0' || customer_id <= 0 ||
                customer_id > 2147483647L ||
                (strcmp(status_text, "0") != 0 &&
                 strcmp(status_text, "1") != 0)) {
                reply(session->socket_fd, "ERR|INVALID_ACCOUNT_STATUS");
                continue;
            }

            int account_id = 0;
            int active = status_text[0] - '0';
            int result = set_account_status((int)customer_id, active,
                                            &account_id);
            if (result == 0) {
                reply(session->socket_fd, "ERR|CUSTOMER_ACCOUNT_NOT_FOUND");
            } else if (result < 0) {
                reply(session->socket_fd, "ERR|ACCOUNT_STATUS_FAILED");
            } else {
                char response[BUFFER_SIZE];
                snprintf(response, sizeof(response), "OK|ACCOUNT_STATUS|%d|%s",
                         account_id, active ? "ACTIVE" : "INACTIVE");
                reply(session->socket_fd, response);
            }
        } else if (strcmp(command, "BALANCE") == 0) {
            if (!require_customer(session))
                continue;

            int account_id;
            long long balance;
            int result = get_account_balance(session->user_id, &account_id, &balance);

            if (result == -1) {
                reply(session->socket_fd, "ERR|ACCOUNT_STORAGE_ERROR");
            } else if (result == 0) {
                reply(session->socket_fd, "ERR|NO_ACTIVE_ACCOUNT");
            } else {
                char response[BUFFER_SIZE];
                snprintf(response, sizeof(response),
                         "OK|BALANCE|%d|%lld", account_id, balance);
                reply(session->socket_fd, response);
            }
        } else if (strcmp(command, "DEPOSIT") == 0 ||
                   strcmp(command, "WITHDRAW") == 0) {
            int is_deposit = (strcmp(command, "DEPOSIT") == 0);

            if (!require_customer(session))
                continue;

            char *amount_text = strtok_r(NULL, "|", &save);
            char *extra = strtok_r(NULL, "|", &save);
            long long amount;

            if (amount_text == NULL || extra != NULL ||
                !parse_positive_amount(amount_text, &amount)) {
                reply(session->socket_fd, "ERR|INVALID_AMOUNT");
                continue;
            }

            int account_id = 0;
            long long balance = 0;
            int result = update_balance(session->user_id, amount, is_deposit,
                                        &account_id, &balance);
            reply_money_result(session->socket_fd, result,
                               is_deposit ? "OK|DEPOSIT|%d|%lld"
                                          : "OK|WITHDRAW|%d|%lld",
                               account_id, balance);
        } else if (strcmp(command, "TRANSFER") == 0) {
            if (!require_customer(session))
                continue;

            char *dest_text = strtok_r(NULL, "|", &save);
            char *amount_text = strtok_r(NULL, "|", &save);
            char *extra = strtok_r(NULL, "|", &save);
            long long amount;

            if (!dest_text || !amount_text || extra) {
                reply(session->socket_fd, "ERR|INVALID_TRANSFER");
                continue;
            }

            char *end = NULL;
            errno = 0;
            long dest_id = strtol(dest_text, &end, 10);
            if (errno || end == dest_text || *end != '\0' ||
                dest_id <= 0 || dest_id > 2147483647L ||
                !parse_positive_amount(amount_text, &amount)) {
                reply(session->socket_fd, "ERR|INVALID_TRANSFER");
                continue;
            }

            int from_id = 0, to_id = 0;
            long long from_bal = 0;
            int result = transfer_funds(session->user_id, (int)dest_id, amount,
                                        &from_id, &to_id, &from_bal);

            if (result == 1) {
                char response[BUFFER_SIZE];
                snprintf(response, sizeof(response),
                         "OK|TRANSFER|%d|%d|%lld", from_id, to_id, from_bal);
                reply(session->socket_fd, response);
            } else if (result == 0) {
                reply(session->socket_fd, "ERR|NO_ACTIVE_ACCOUNT");
            } else if (result == -2) {
                reply(session->socket_fd, "ERR|INSUFFICIENT_FUNDS");
            } else if (result == -3) {
                reply(session->socket_fd, "ERR|DEST_NOT_FOUND");
            } else if (result == -4) {
                reply(session->socket_fd, "ERR|SAME_ACCOUNT");
            } else if (result == -5) {
                reply(session->socket_fd, "ERR|TXN_LOG_FAILED");
            } else {
                reply(session->socket_fd, "ERR|ACCOUNT_UPDATE_FAILED");
            }
        } else if (strcmp(command, "HISTORY") == 0) {
            if (!require_customer(session))
                continue;

            int account_id;
            long long balance;
            int found = get_account_balance(session->user_id, &account_id, &balance);
            if (found == -1) {
                reply(session->socket_fd, "ERR|ACCOUNT_STORAGE_ERROR");
                continue;
            }
            if (found == 0) {
                reply(session->socket_fd, "ERR|NO_ACTIVE_ACCOUNT");
                continue;
            }

            send_transaction_history(session->socket_fd, account_id);
        } else if (strcmp(command, "PASSWD") == 0) {
            if (!session->authenticated) {
                reply(session->socket_fd, "ERR|LOGIN_REQUIRED");
                continue;
            }

            char *old_pass = strtok_r(NULL, "|", &save);
            char *new_pass = strtok_r(NULL, "|", &save);
            char *extra = strtok_r(NULL, "|", &save);

            if (!old_pass || !new_pass || extra ||
                old_pass[0] == '\0' || new_pass[0] == '\0' ||
                strlen(new_pass) >= 128) {
                reply(session->socket_fd, "ERR|INVALID_PASSWORD");
                continue;
            }

            int result = change_password(session->user_id, old_pass, new_pass);
            if (result == 1)
                reply(session->socket_fd, "OK|PASSWD");
            else if (result == 0)
                reply(session->socket_fd, "ERR|BAD_PASSWORD");
            else
                reply(session->socket_fd, "ERR|USER_STORAGE_ERROR");
        } else if (strcmp(command, "FEEDBACK") == 0) {
            if (!require_customer(session))
                continue;

            const char *text = NULL;
            if (strncmp(buffer, "FEEDBACK|", 9) == 0)
                text = buffer + 9;

            if (!text || text[0] == '\0' || strchr(text, '|') != NULL) {
                reply(session->socket_fd, "ERR|INVALID_FEEDBACK");
                continue;
            }

            int feedback_id = 0;
            if (add_feedback(session->user_id, text, &feedback_id) != 1) {
                reply(session->socket_fd, "ERR|FEEDBACK_FAILED");
                continue;
            }

            char response[BUFFER_SIZE];
            snprintf(response, sizeof(response), "OK|FEEDBACK|%d", feedback_id);
            reply(session->socket_fd, response);
        } else if (strcmp(command, "LOAN") == 0) {
            if (!require_customer(session))
                continue;

            char *amount_text = strtok_r(NULL, "|", &save);
            char *extra = strtok_r(NULL, "|", &save);
            long long amount;

            if (!amount_text || extra || !parse_positive_amount(amount_text, &amount)) {
                reply(session->socket_fd, "ERR|INVALID_AMOUNT");
                continue;
            }

            int loan_id = 0;
            if (apply_loan(session->user_id, amount, &loan_id) != 1) {
                reply(session->socket_fd, "ERR|LOAN_FAILED");
                continue;
            }

            char response[BUFFER_SIZE];
            snprintf(response, sizeof(response), "OK|LOAN|%d", loan_id);
            reply(session->socket_fd, response);
        } else if (strcmp(command, "WHOAMI") == 0) {
            if (!session->authenticated) {
                reply(session->socket_fd, "ERR|LOGIN_REQUIRED");
                continue;
            }

            char response[BUFFER_SIZE];
            snprintf(response, sizeof(response),
                     "OK|%d|%s", session->user_id, role_name(session->role));
            reply(session->socket_fd, response);
        } else if (strcmp(command, "LOGOUT") == 0) {
            if (!session->authenticated) {
                reply(session->socket_fd, "ERR|NOT_LOGGED_IN");
                continue;
            }

            release_session(session->user_id);
            session->user_id = 0;
            session->role = ROLE_NONE;
            session->authenticated = 0;
            reply(session->socket_fd, "OK|LOGOUT");
        } else if (strcmp(command, "EXIT") == 0) {
            reply(session->socket_fd, "OK|BYE");
            break;
        } else {
            reply(session->socket_fd, "ERR|UNKNOWN_COMMAND");
        }
    }

    release_session(session->user_id);
    printf("Client disconnected (fd=%d)\n", session->socket_fd);
    close(session->socket_fd);
    free(session);
    return NULL;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    if (init_users() < 0) {
        perror("user storage initialization");
        return EXIT_FAILURE;
    }
    init_auth();
    if (init_accounts() < 0) {
        perror("account storage initialization");
        return EXIT_FAILURE;
    }
    if (init_loans() < 0) {
        perror("loan storage initialization");
        return EXIT_FAILURE;
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(server_fd);
        return EXIT_FAILURE;
    }

    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(PORT);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind");
        close(server_fd);
        return EXIT_FAILURE;
    }

    if (listen(server_fd, 10) < 0) {
        perror("listen");
        close(server_fd);
        return EXIT_FAILURE;
    }

    printf("Bank server listening on 127.0.0.1:%d\n", PORT);

    while (1) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            continue;
        }

        ClientSession *session = calloc(1, sizeof(*session));
        if (!session) {
            perror("calloc");
            close(client_fd);
            continue;
        }

        session->socket_fd = client_fd;

        pthread_t thread;
        pthread_attr_t attr;
        int rc = pthread_attr_init(&attr);
        if (rc == 0) {
            rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            if (rc == 0)
                rc = pthread_create(&thread, &attr, handle_client, session);
            pthread_attr_destroy(&attr);
        }

        if (rc != 0) {
            fprintf(stderr, "Thread creation failed: %s\n", strerror(rc));
            close(client_fd);
            free(session);
        }
    }

    close(server_fd);
    return EXIT_SUCCESS;
}
