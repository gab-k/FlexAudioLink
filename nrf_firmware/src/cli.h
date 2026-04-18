#pragma once

#include <stddef.h>
#include <stdbool.h>

#define CLI_MAX_CMD_LEN 128
#define CLI_MAX_OUTPUT_LEN 512

void cli_set_connected(bool connected);
void cli_enqueue_command_line(char *line);
void cli_enqueue_print_msg(const char *message);
