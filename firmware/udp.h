#ifndef _UDP_TASKS_H_
#define _UDP_TASKS_H_

#include "tusb.h"
#include "task.h"

extern TaskHandle_t g_udp_task_handle;

void udp_audio_ff_init(void);
tu_fifo_t* udp_get_spk_fifo(void);
tu_fifo_t* udp_get_mic_fifo(void);
void udp_task(void *pvParameters);

/**
 * @brief Updates the pending feedback value to be sent to the Dongle.
 * @note This overwrites any previously unsent feedback value (Last-Write-Wins).
 */
void udp_queue_feedback(uint32_t value_16_16);

#endif /* _UDP_TASKS_H_ */