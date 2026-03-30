#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <hal/nrf_radio.h>
#include <zephyr/kernel.h>

#define SYNC_WORD           0xD391A5D3UL
#define FREQ                2480
#define TXPOWER             NRF_RADIO_TXPOWER_POS8DBM
#define MODE                NRF_RADIO_MODE_NRF_4MBIT_BT_0_6
#define INTERVAL_MS         1
#define PAYLOAD_LEN         250
#define PACKET_LEN          (2 + PAYLOAD_LEN)
#define TX_DEVICE_ID        0xb007ec2fe3f4e0c0ULL

struct radio_packet {
	uint16_t seq;
	uint8_t data[PAYLOAD_LEN];
} __packed __aligned(4);

enum radio_runtime_mode {
	RADIO_RUNTIME_OFF,
	RADIO_RUNTIME_TX,
	RADIO_RUNTIME_RX,
};

struct radio_stats {
	uint32_t packets_tx;
	uint32_t packets_rx;
	uint32_t packets_lost;
	int16_t last_rssi_dbm;
	bool peer_connected;
};

extern struct radio_packet pkt_tx;
extern struct radio_packet pkt_rx[2];
extern volatile uint8_t rx_buf_idx;

extern struct k_sem tx_done_sem;
extern struct k_sem rx_sem;
extern volatile int16_t rx_rssi;
extern volatile uint8_t rx_ready_idx;

void radio_hw_init(void);
void radio_hw_set_runtime_mode(enum radio_runtime_mode mode);
enum radio_runtime_mode radio_hw_get_runtime_mode(void);
void radio_hw_get_stats(struct radio_stats *stats);
void radio_hw_reset_stats(void);
void radio_hw_record_tx(void);
void radio_hw_record_rx(uint16_t seq, int16_t rssi_dbm);
