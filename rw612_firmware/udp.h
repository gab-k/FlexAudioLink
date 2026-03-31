#ifndef _UDP_TASKS_H_
#define _UDP_TASKS_H_

#include "tusb.h"
#include "task.h"

// Latency Tuning:
// 48kHz * 2 channels * 2 bytes = 192 bytes per millisecond.
// 512 bytes = 2.66 ms
// 480 bytes = 2.5 ms
// 240 bytes = 1.25 ms
// 192 bytes = 1 ms
#define UDP_PACKET_SIZE  (192*2)

extern TaskHandle_t g_udp_rx_task_handle;
extern TaskHandle_t g_udp_tx_task_handle;

void udp_audio_ff_init(void);
tu_fifo_t* udp_get_spk_fifo(void);
tu_fifo_t* udp_get_mic_fifo(void);
void udp_rx_task(void *pvParameters);
void udp_tx_task(void *pvParameters);

/**
 * @brief Updates the pending feedback value to be sent to the Dongle.
 * @note This overwrites any previously unsent feedback value (Last-Write-Wins).
 */
void udp_queue_feedback(uint32_t value_16_16);

#endif /* _UDP_TASKS_H_ */