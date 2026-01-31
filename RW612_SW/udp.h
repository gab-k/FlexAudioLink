#ifndef _UDP_TASKS_H_
#define _UDP_TASKS_H_

#include "lwip/sockets.h"
#include "stdio.h"
#include "pin_mux.h"
#include "fsl_gpio.h"
#include "wifi_app.h"
#include "tusb.h"

extern TaskHandle_t g_udp_task_handle;

void udp_audio_ff_init(void);
tu_fifo_t* udp_get_spk_fifo(void);
tu_fifo_t* udp_get_mic_fifo(void);
void udp_task(void *pvParameters);

#endif /* _UDP_TASKS_H_ */