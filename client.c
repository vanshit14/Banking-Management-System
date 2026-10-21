#include "common.h"
#include "amount.h"

#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
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
        if (n == 0)
            return -1;
        sent += (size_t)n;
    }
    return 0;
}

static int read_line(int fd, char *buffer, size_t capacity) {
    size_t used = 0;

    while (used < capacity - 1) {
        char ch;
        ssize_t n = recv(fd, &ch, 1, 0);
        if (n == 0)
            return 0;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (ch == '\n') {
            buffer[used] = '\0';
            return 1;
        }
        if (ch == '\r' || ch == '\0')
            return -1;
        buffer[used++] = ch;
    }
    return -1;
}

static int read_input(const char *prompt, char *buffer, size_t capacity) {
    if (write(STDOUT_FILENO, prompt, strlen(prompt)) < 0)
        return -1;

    if (capacity < 2)
        return -1;

    size_t used = 0;
    int too_long = 0;

    while (1) {
        char ch;
        ssize_t n = read(STDIN_FILENO, &ch, 1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) {
            if (used == 0)
                return 0;
            break;
        }
        if (ch == '\n')
            break;
        if (ch == '\r')
            continue;
        if (used < capacity - 1)
            buffer[used++] = ch;
        else
            too_long = 1;
    }

    buffer[used] = '\0';

    if (too_long) {
        const char msg[] = "Input too long. Try again.\n";
        (void)write(STDOUT_FILENO, msg, sizeof(msg) - 1);
        return -1;
    }
    return 1;
}

static int read_password(const char *prompt, char *password, size_t capacity) {
    struct termios old_settings, new_settings;

    if (tcgetattr(STDIN_FILENO, &old_settings) != 0)
        return read_input(prompt, password, capacity);

    new_settings = old_settings;
    new_settings.c_lflag &= (tcflag_t)~ECHO;

    if (write(STDOUT_FILENO, prompt, strlen(prompt)) < 0)
        return -1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &new_settings) != 0)
        return -1;

    int result = read_input("", password, capacity);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_settings);
    printf("\n");
    return result;
}

static int send_request(int fd, const char *command,
                        char *response, size_t capacity) {
    if (send_all(fd, command, strlen(command)) < 0 ||
        send_all(fd, "\n", 1) < 0) {
        perror("send");
        return -1;
    }

    int result = read_line(fd, response, capacity);
    if (result != 1) {
        if (result < 0)
            perror("recv");
        else
            printf("Server disconnected.\n");
        return -1;
    }

    printf("\n%s\n", response);
    return 0;
}

static int send_list_request(int fd, const char *command,
                             const char *header_prefix) {
    if (send_all(fd, command, strlen(command)) < 0 ||
        send_all(fd, "\n", 1) < 0) {
        perror("send");
        return -1;
    }

    char response[BUFFER_SIZE];
    if (read_line(fd, response, sizeof(response)) != 1) {
        printf("Server disconnected.\n");
        return -1;
    }

    printf("\n%s\n", response);
    size_t prefix_length = strlen(header_prefix);
    if (strncmp(response, header_prefix, prefix_length) != 0)
        return 0;

    char *end = NULL;
    long count = strtol(response + prefix_length, &end, 10);
    if (end == response + prefix_length || *end != '\0' || count < 0)
        return -1;

    for (long i = 0; i < count; i++) {
        if (read_line(fd, response, sizeof(response)) != 1) {
            printf("Server disconnected.\n");
            return -1;
        }
        printf("%s\n", response);
    }

    if (read_line(fd, response, sizeof(response)) != 1) {
        printf("Server disconnected.\n");
        return -1;
    }
    printf("%s\n", response);
    return 0;
}

static int parse_amount(const char *text, long long *amount) {
    if (!parse_money_amount(text, amount)) {
        printf("Enter a positive whole-number amount (maximum 1000000000000000).\n");
        return 0;
    }
    return 1;
}

int main(void) {
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(PORT);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(sock_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("connect");
        close(sock_fd);
        return EXIT_FAILURE;
    }

    printf("=================================\n");
    printf("   BANKING MANAGEMENT SYSTEM\n");
    printf("=================================\n");

    int logged_in = 0;
    int is_customer = 0;
    int is_employee = 0;
    int is_manager = 0;
    int running = 1;
    char input[BUFFER_SIZE];
    char command[BUFFER_SIZE];
    char response[BUFFER_SIZE];

    while (running) {
        if (!logged_in) {
            printf("\n1. Login\n");
            printf("2. Exit\n");

            int rc = read_input("Enter choice: ", input, sizeof(input));
            if (rc == 0)
                break;
            if (rc < 0)
                continue;

            if (strcmp(input, "1") == 0) {
                char username[128];
                char password[128];

                rc = read_input("Username: ", username, sizeof(username));
                if (rc == 0)
                    break;
                if (rc < 0)
                    continue;

                rc = read_password("Password: ", password, sizeof(password));
                if (rc == 0)
                    break;
                if (rc < 0)
                    continue;

                if (strchr(username, '|') || strchr(password, '|') ||
                    username[0] == '\0' || password[0] == '\0') {
                    printf("Invalid username or password format.\n");
                    continue;
                }

                int n = snprintf(command, sizeof(command),
                                 "LOGIN|%s|%s", username, password);
                memset(password, 0, sizeof(password));
                if (n < 0 || (size_t)n >= sizeof(command)) {
                    printf("Input too long.\n");
                    continue;
                }

                if (send_request(sock_fd, command, response, sizeof(response)) < 0) {
                    running = 0;
                    break;
                }

                logged_in = strncmp(response, "OK|LOGIN|", 9) == 0;
                is_customer = 0;
                is_employee = 0;
                is_manager = 0;
                if (logged_in) {
                    char *role = strrchr(response, '|');
                    is_customer = role && strcmp(role + 1, "CUSTOMER") == 0;
                    is_employee = role && strcmp(role + 1, "EMPLOYEE") == 0;
                    is_manager = role && strcmp(role + 1, "MANAGER") == 0;
                }
                memset(command, 0, sizeof(command));
            } else if (strcmp(input, "2") == 0) {
                if (send_request(sock_fd, "EXIT", response, sizeof(response)) < 0) {
                    running = 0;
                    break;
                }
                running = 0;
            } else {
                printf("Invalid choice. Try again.\n");
            }
        } else if (is_employee) {
            printf("\n-------- EMPLOYEE MENU --------\n");
            printf("1. Add New Customer\n");
            printf("2. Modify Customer Username\n");
            printf("3. View Customer Transactions\n");
            printf("4. View Assigned Loans\n");
            printf("5. Logout\n");
            printf("6. Exit\n");

            int rc = read_input("Enter choice: ", input, sizeof(input));
            if (rc == 0)
                break;
            if (rc < 0)
                continue;

            if (strcmp(input, "1") == 0) {
                char username[128], password[128], balance_text[128];
                rc = read_input("New username: ", username, sizeof(username));
                if (rc <= 0) {
                    if (rc == 0) running = 0;
                    continue;
                }
                rc = read_password("Initial password: ", password,
                                   sizeof(password));
                if (rc <= 0) {
                    if (rc == 0) running = 0;
                    continue;
                }
                rc = read_input("Opening balance: ", balance_text,
                                sizeof(balance_text));
                if (rc <= 0) {
                    memset(password, 0, sizeof(password));
                    if (rc == 0) running = 0;
                    continue;
                }
                long long opening_balance;
                if (!parse_amount(balance_text, &opening_balance) ||
                    username[0] == '\0' || password[0] == '\0' ||
                    strchr(username, '|') || strchr(password, '|')) {
                    printf("Invalid customer details.\n");
                    memset(password, 0, sizeof(password));
                    continue;
                }
                int n = snprintf(command, sizeof(command),
                                 "ADD_CUSTOMER|%s|%s|%lld",
                                 username, password, opening_balance);
                memset(password, 0, sizeof(password));
                if (n < 0 || (size_t)n >= sizeof(command)) {
                    printf("Customer details are too long.\n");
                    continue;
                }
            } else if (strcmp(input, "2") == 0) {
                char customer_id[32], new_username[128];
                rc = read_input("Customer user id: ", customer_id,
                                sizeof(customer_id));
                if (rc <= 0) {
                    if (rc == 0) running = 0;
                    continue;
                }
                rc = read_input("New username: ", new_username,
                                sizeof(new_username));
                if (rc <= 0) {
                    if (rc == 0) running = 0;
                    continue;
                }
                if (new_username[0] == '\0' || strchr(new_username, '|') ||
                    strchr(customer_id, '|')) {
                    printf("Invalid customer details.\n");
                    continue;
                }
                int n = snprintf(command, sizeof(command),
                                 "MODIFY_CUSTOMER|%s|%s",
                                 customer_id, new_username);
                if (n < 0 || (size_t)n >= sizeof(command)) {
                    printf("Customer details are too long.\n");
                    continue;
                }
            } else if (strcmp(input, "3") == 0) {
                char customer_id[32];
                rc = read_input("Customer user id: ", customer_id,
                                sizeof(customer_id));
                if (rc <= 0) {
                    if (rc == 0) running = 0;
                    continue;
                }
                if (strchr(customer_id, '|')) {
                    printf("Invalid customer id.\n");
                    continue;
                }
                int n = snprintf(command, sizeof(command),
                                 "CUSTOMER_HISTORY|%s", customer_id);
                if (n < 0 || (size_t)n >= sizeof(command)) {
                    printf("Invalid customer id.\n");
                    continue;
                }
                if (send_list_request(sock_fd, command, "OK|HISTORY|") < 0)
                    running = 0;
                memset(command, 0, sizeof(command));
                continue;
            } else if (strcmp(input, "4") == 0) {
                if (send_list_request(sock_fd, "ASSIGNED_LOANS",
                                      "OK|LOANS|") < 0)
                    running = 0;
                continue;
            } else if (strcmp(input, "5") == 0) {
                strcpy(command, "LOGOUT");
            } else if (strcmp(input, "6") == 0) {
                strcpy(command, "EXIT");
            } else {
                printf("Invalid choice. Try again.\n");
                continue;
            }

            if (send_request(sock_fd, command, response, sizeof(response)) < 0) {
                running = 0;
                break;
            }
            if (strcmp(command, "LOGOUT") == 0 &&
                strcmp(response, "OK|LOGOUT") == 0) {
                logged_in = 0;
                is_customer = 0;
                is_employee = 0;
            }
            if (strcmp(command, "EXIT") == 0)
                running = 0;
            memset(command, 0, sizeof(command));
        } else if (is_manager) {
            printf("\n--------- MANAGER MENU ---------\n");
            printf("1. Activate Customer Account\n");
            printf("2. Deactivate Customer Account\n");
            printf("3. Assign Loan to Employee\n");
            printf("4. Logout\n");
            printf("5. Exit\n");

            int rc = read_input("Enter choice: ", input, sizeof(input));
            if (rc == 0)
                break;
            if (rc < 0)
                continue;

            if (strcmp(input, "1") == 0 || strcmp(input, "2") == 0) {
                char customer_id[32];
                rc = read_input("Customer user id: ", customer_id,
                                sizeof(customer_id));
                if (rc <= 0) {
                    if (rc == 0) running = 0;
                    continue;
                }
                if (strchr(customer_id, '|')) {
                    printf("Invalid customer id.\n");
                    continue;
                }
                int n = snprintf(command, sizeof(command),
                                 "SET_ACCOUNT_STATUS|%s|%s", customer_id,
                                 strcmp(input, "1") == 0 ? "1" : "0");
                if (n < 0 || (size_t)n >= sizeof(command)) {
                    printf("Invalid customer id.\n");
                    continue;
                }
            } else if (strcmp(input, "3") == 0) {
                char loan_id[32], employee_id[32];
                rc = read_input("Loan id: ", loan_id, sizeof(loan_id));
                if (rc <= 0) {
                    if (rc == 0) running = 0;
                    continue;
                }
                rc = read_input("Employee user id: ", employee_id,
                                sizeof(employee_id));
                if (rc <= 0) {
                    if (rc == 0) running = 0;
                    continue;
                }
                if (strchr(loan_id, '|') || strchr(employee_id, '|')) {
                    printf("Invalid loan assignment.\n");
                    continue;
                }
                int n = snprintf(command, sizeof(command),
                                 "ASSIGN_LOAN|%s|%s", loan_id, employee_id);
                if (n < 0 || (size_t)n >= sizeof(command)) {
                    printf("Invalid loan assignment.\n");
                    continue;
                }
            } else if (strcmp(input, "4") == 0) {
                strcpy(command, "LOGOUT");
            } else if (strcmp(input, "5") == 0) {
                strcpy(command, "EXIT");
            } else {
                printf("Invalid choice. Try again.\n");
                continue;
            }

            if (send_request(sock_fd, command, response, sizeof(response)) < 0) {
                running = 0;
                break;
            }
            if (strcmp(command, "LOGOUT") == 0 &&
                strcmp(response, "OK|LOGOUT") == 0) {
                logged_in = 0;
                is_manager = 0;
            }
            if (strcmp(command, "EXIT") == 0)
                running = 0;
            memset(command, 0, sizeof(command));
        } else if (!is_customer) {
            printf("\n--------- STAFF MENU ----------\n");
            printf("Role features are not implemented yet.\n");
            printf("1. Logout\n");
            printf("2. Exit\n");

            int rc = read_input("Enter choice: ", input, sizeof(input));
            if (rc == 0)
                break;
            if (rc < 0)
                continue;

            if (strcmp(input, "1") == 0)
                strcpy(command, "LOGOUT");
            else if (strcmp(input, "2") == 0)
                strcpy(command, "EXIT");
            else {
                printf("Invalid choice. Try again.\n");
                continue;
            }

            if (send_request(sock_fd, command, response, sizeof(response)) < 0) {
                running = 0;
                break;
            }
            if (strcmp(command, "LOGOUT") == 0 &&
                strcmp(response, "OK|LOGOUT") == 0) {
                logged_in = 0;
                is_employee = 0;
            }
            if (strcmp(command, "EXIT") == 0)
                running = 0;
        } else {
            printf("\n--------- CUSTOMER MENU ---------\n");
            printf("1. View Balance\n");
            printf("2. Deposit Money\n");
            printf("3. Withdraw Money\n");
            printf("4. Transfer Funds\n");
            printf("5. View Transaction History\n");
            printf("6. Apply for a Loan\n");
            printf("7. Change Password\n");
            printf("8. Add Feedback\n");
            printf("9. Logout\n");
            printf("10. Exit\n");

            int rc = read_input("Enter choice: ", input, sizeof(input));
            if (rc == 0)
                break;
            if (rc < 0)
                continue;

            command[0] = '\0';

            if (strcmp(input, "1") == 0) {
                strcpy(command, "BALANCE");
            } else if (strcmp(input, "2") == 0 || strcmp(input, "3") == 0) {
                int deposit = strcmp(input, "2") == 0;
                char amount_text[128];
                rc = read_input("Enter amount: ", amount_text, sizeof(amount_text));
                if (rc == 0)
                    break;
                if (rc < 0)
                    continue;

                long long amount;
                if (!parse_amount(amount_text, &amount))
                    continue;

                int n = snprintf(command, sizeof(command), "%s|%lld",
                                 deposit ? "DEPOSIT" : "WITHDRAW", amount);
                if (n < 0 || (size_t)n >= sizeof(command)) {
                    printf("Invalid amount.\n");
                    continue;
                }
            } else if (strcmp(input, "4") == 0) {
                char dest_text[128];
                char amount_text[128];
                rc = read_input("Destination account id: ",
                                dest_text, sizeof(dest_text));
                if (rc == 0)
                    break;
                if (rc < 0)
                    continue;
                rc = read_input("Enter amount: ", amount_text, sizeof(amount_text));
                if (rc == 0)
                    break;
                if (rc < 0)
                    continue;

                long long amount;
                if (!parse_amount(amount_text, &amount))
                    continue;

                int n = snprintf(command, sizeof(command),
                                 "TRANSFER|%s|%lld", dest_text, amount);
                if (n < 0 || (size_t)n >= sizeof(command) ||
                    strchr(dest_text, '|')) {
                    printf("Invalid transfer.\n");
                    continue;
                }
            } else if (strcmp(input, "5") == 0) {
                if (send_list_request(sock_fd, "HISTORY", "OK|HISTORY|") < 0) {
                    running = 0;
                    break;
                }
                continue;
            } else if (strcmp(input, "6") == 0) {
                char amount_text[128];
                rc = read_input("Loan amount: ", amount_text, sizeof(amount_text));
                if (rc == 0)
                    break;
                if (rc < 0)
                    continue;

                long long amount;
                if (!parse_amount(amount_text, &amount))
                    continue;

                int n = snprintf(command, sizeof(command), "LOAN|%lld", amount);
                if (n < 0 || (size_t)n >= sizeof(command)) {
                    printf("Invalid amount.\n");
                    continue;
                }
            } else if (strcmp(input, "7") == 0) {
                char old_pass[128];
                char new_pass[128];
                rc = read_password("Current password: ", old_pass, sizeof(old_pass));
                if (rc == 0)
                    break;
                if (rc < 0)
                    continue;
                rc = read_password("New password: ", new_pass, sizeof(new_pass));
                if (rc == 0)
                    break;
                if (rc < 0)
                    continue;

                if (strchr(old_pass, '|') || strchr(new_pass, '|') ||
                    old_pass[0] == '\0' || new_pass[0] == '\0') {
                    printf("Invalid password format.\n");
                    continue;
                }

                int n = snprintf(command, sizeof(command),
                                 "PASSWD|%s|%s", old_pass, new_pass);
                memset(old_pass, 0, sizeof(old_pass));
                memset(new_pass, 0, sizeof(new_pass));
                if (n < 0 || (size_t)n >= sizeof(command)) {
                    printf("Input too long.\n");
                    continue;
                }
            } else if (strcmp(input, "8") == 0) {
                char text[400];
                rc = read_input("Feedback: ", text, sizeof(text));
                if (rc == 0)
                    break;
                if (rc < 0)
                    continue;
                if (text[0] == '\0' || strchr(text, '|')) {
                    printf("Feedback cannot be empty or contain '|'.\n");
                    continue;
                }
                int n = snprintf(command, sizeof(command), "FEEDBACK|%s", text);
                if (n < 0 || (size_t)n >= sizeof(command)) {
                    printf("Feedback too long.\n");
                    continue;
                }
            } else if (strcmp(input, "9") == 0) {
                strcpy(command, "LOGOUT");
            } else if (strcmp(input, "10") == 0) {
                strcpy(command, "EXIT");
            } else {
                printf("Invalid choice. Try again.\n");
                continue;
            }

            if (send_request(sock_fd, command, response, sizeof(response)) < 0) {
                running = 0;
                break;
            }

            if (strcmp(command, "LOGOUT") == 0 &&
                strcmp(response, "OK|LOGOUT") == 0) {
                logged_in = 0;
                is_customer = 0;
            }
            if (strcmp(command, "EXIT") == 0)
                running = 0;

            memset(command, 0, sizeof(command));
        }
    }

    close(sock_fd);
    printf("Thank you for using the banking system.\n");
    return EXIT_SUCCESS;
}
