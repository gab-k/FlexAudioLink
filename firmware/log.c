#include "log.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include <stdio.h>
#include <stdarg.h>

// NXP Drivers
#include "fsl_usart.h"
#include "fsl_debug_console.h"

// --- Configuration ---
#define LOG_QUEUE_LENGTH    20      // How many messages can buffer before dropping
#define LOG_MAX_MSG_LEN     128     // Max characters per line
#define LOG_UART_BASE       USART3

// --- Data Types ---
typedef struct {
    uint16_t length;
    char buffer[LOG_MAX_MSG_LEN];
} log_msg_t;

static QueueHandle_t log_q = NULL;


void log_init_q(void) {
    if (log_q == NULL) {
        log_q = xQueueCreate(LOG_QUEUE_LENGTH, sizeof(log_msg_t));
    }
}

void log_task(void *pvParameters) {
    log_msg_t rx_msg;
    if (log_q == NULL) {
        // Use the Debug consoles printf for this emergency alert
        DbgConsole_Printf("ERROR: loq_q == NULL, was log_init_q() called?\r\n");

        // Terminate this task so it doesn't crash the scheduler
        vTaskDelete(NULL);
    }
    while (1) {
        // Block here indefinitely until a message arrives
        if (xQueueReceive(log_q, &rx_msg, portMAX_DELAY) == pdPASS) {
            // TODO: Consider DMA write here in the future
            USART_WriteBlocking(LOG_UART_BASE, (uint8_t *)rx_msg.buffer, rx_msg.length);
        }
    }
}

// Called by tasks to queue log messages
void log_print(const char *format, ...) {
    static bool init_warn_shown = false;

    if (log_q == NULL) {
        if (!init_warn_shown) {
            // Use the Debug consoles printf for this emergency alert
            DbgConsole_Printf("ERROR: loq_q == NULL, was log_init_q() called?\r\n");
            init_warn_shown = true;
        }
        return;
    }

    log_msg_t msg;
    
    // SAFETY: We format the string HERE to capture stack variables immediately.
    // If we passed pointers, the data might be gone by the time the daemon runs.
    va_list args;
    va_start(args, format);
    
    // vsnprintf protects against buffer overflows
    int len = vsnprintf(msg.buffer, LOG_MAX_MSG_LEN, format, args);
    if (len >= 0) {
        // If len > sizeof(buffer), it was truncated, so we cap it
        msg.length = (len < sizeof(msg.buffer)) ? (uint16_t)len : (uint16_t)(sizeof(msg.buffer) - 1);
    }
    va_end(args);

    // Check for formatting errors
    if (len < 0) {
        // Use the Debug consoles printf for this emergency alert
        DbgConsole_Printf("ERROR: vsnprintf failed with code: %d\r\n", len);
        return;
    }

    // Send to Queue
    // 0 wait time = If queue is full, drop the message.
    if (xPortIsInsideInterrupt()) {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xQueueSendFromISR(log_q, &msg, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    } else {
        xQueueSend(log_q, &msg, 0);
    }
}