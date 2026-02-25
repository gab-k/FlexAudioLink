#ifndef _WIFI_APP_H_
#define _WIFI_APP_H_

#include "FreeRTOS.h"
#include "task.h"
#include "event_groups.h"

// Event Bits
#define WIFI_INIT_DONE  (1 << 0) // IP Acquired (AP or STA)

extern TaskHandle_t g_wifi_init_task_handle;
extern EventGroupHandle_t g_wifi_events;

void wifi_init_task(void *pvParameters);

#endif /* _WIFI_APP_H_ */