#ifndef _RAW_AUDIO_H_
#define _RAW_AUDIO_H_

#include "tusb.h"
#include "task.h"
#include "lwip/netif.h"

// Packet size must match UDP_PACKET_SIZE in udp.h: 192 bytes/ms * 2ms = 384 bytes
#define RAW_AUDIO_PACKET_SIZE (192 * 2)

extern TaskHandle_t g_raw_rx_task_handle;
extern TaskHandle_t g_raw_tx_task_handle;

void raw_audio_ff_init(void);
tu_fifo_t *raw_get_spk_fifo(void);
tu_fifo_t *raw_get_mic_fifo(void);

void raw_rx_task(void *pvParameters);
void raw_tx_task(void *pvParameters);

void raw_queue_feedback(uint32_t value_16_16);

// Install the netif RX intercept hook. Must be called from a task (not ISR).
// Safe to call multiple times — only installs once per netif.
void raw_audio_install_hook(struct netif *netif);

// Set the peer (destination) MAC address for TX.
// Dongle: learns from WLAN STA list after headset connects.
// Headset: set from AP BSSID after association.
void raw_audio_set_peer_mac(const uint8_t *mac);

// Clear peer MAC validity — call at the start of wifi_init_task's loop,
// before setup, so stale MACs from a previous connection are invalidated.
void raw_audio_reset_peer_mac(void);

#endif /* _RAW_AUDIO_H_ */
