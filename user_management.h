#ifndef USER_MANAGEMENT_H
#define USER_MANAGEMENT_H

#include "common.h"

/* Returns 1 on success, -2 if the username exists, and -1 on failure. */
int add_employee(const char *username, const char *password, int *user_id);
int modify_username_for_role(int user_id, const char *new_username,
                             Role required_role);
int admin_modify_username(int user_id, const char *new_username);
int set_staff_role(int user_id, Role new_role);

#endif
