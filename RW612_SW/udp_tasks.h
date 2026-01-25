#ifndef _UDP_TASKS_H_
#define _UDP_TASKS_H_

#include "lwip/sockets.h"
#include "stdio.h"
#include "pin_mux.h"
#include "fsl_gpio.h"
#include "wifi_app.h"

extern TaskHandle_t g_udp_task_handle;

void udp_task(void *pvParameters);

#endif /* _UDP_TASKS_H_ */