#ifndef LOG_H
#define LOG_H

void log_init_q(void);
void log_task(void *pvParameters);
void log_print(const char *format, ...);

// Redirect PRINTF to our new function
// #undef PRINTF
// #define PRINTF(...) log_print(__VA_ARGS__)

#endif // LOG_H