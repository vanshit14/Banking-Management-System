#ifndef FEEDBACK_H
#define FEEDBACK_H

#include "common.h"

int add_feedback(int user_id, const char *text, int *feedback_id);
int list_feedback(char rows[][BUFFER_SIZE], int max_rows, int *count);

#endif
