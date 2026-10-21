#ifndef AUTH_H
#define AUTH_H

#include "common.h"
#include <stdint.h>

typedef struct {
    int32_t user_id;
    char username[128];
    char password[128];
    int32_t role;
    int32_t active;
    int32_t reserved;
} UserRecord;

void init_auth(void);
int init_users(void);

const char *role_name(Role role);
Role parse_role(const char *text);

int authenticate_user(const char *username,
                      const char *password,
                      int *user_id,
                      Role *role);

int change_password(int user_id,
                    const char *old_password,
                    const char *new_password);

int reserve_session(int user_id);
void release_session(int user_id);

#endif
