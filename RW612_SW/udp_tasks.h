#ifndef _UDP_TASKS_H_
#define _UDP_TASKS_H_

#include "lwip/sockets.h"
#include "stdio.h"
#include "pin_mux.h"
#include "fsl_gpio.h"

void udp_client_task(void *pvParameters);
void udp_server_task(void *pvParameters);

#endif /* _UDP_TASKS_H_ */