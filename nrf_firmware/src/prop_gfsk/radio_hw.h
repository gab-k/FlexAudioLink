#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <hal/nrf_dppi.h>
#include <hal/nrf_radio.h>
#include <hal/nrf_timer.h>
#include <zephyr/kernel.h>

/* Radio config */
#define PGFSK_HW_FREQUENCY_MHZ       2480
#define PGFSK_HW_TXPOWER             NRF_RADIO_TXPOWER_POS8DBM
#define PGFSK_HW_MODE_SETTING        NRF_RADIO_MODE_NRF_4MBIT_BT_0_6
#define PGFSK_HW_ADDR_DONGLE             0x55D391A5UL
#define PGFSK_HW_ADDR_HEADSET            0x55D391A5UL
#define PGFSK_HW_MAX_PACKET_AIRTIME_US   550U

/* Packet layout */
#define PGFSK_PAYLOAD_MAX_LEN           252
#define PGFSK_PACKET_METADATA_LEN       2U   /* seq(2) */

#define PGFSK_KEEPALIVE_PAYLOAD_LEN     16U
#define PGFSK_KEEPALIVE_SEQ             UINT16_MAX
#define PGFSK_KEEPALIVE_LEN  (PGFSK_PACKET_METADATA_LEN + PGFSK_KEEPALIVE_PAYLOAD_LEN)

/*
 * Optional PHYEND scope probes. Enable by defining PGFSK_HW_DEBUG_PHYEND_GPIO.
 *
 * DEBUG CODE pins:
 *   P0.00: RADIO PHYEND DPPI/PPIB route through GPIOTE30 channel 3
 *   P0.01: firmware-observed RADIO PHYEND in the ISR
 */
#define PGFSK_HW_DEBUG_PHYEND_GPIO

/*
 * TIMER10 runs free at 1 MHz (prescaler 5 on a 32 MHz base clock).
 * 1 tick = 1 µs.  32-bit counter wraps after ~4295 s.
 */
#define PGFSK_TIMER                     NRF_TIMER10
#define PGFSK_TIMER_IRQn                TIMER10_IRQn
#define PGFSK_TIMER_FREQ_HZ             1000000U
#define PGFSK_TIMER_PRESCALER           5U
#define PGFSK_DPPI                      NRF_DPPIC10

/*
 * DPPI channel assignments (within DPPIC10):
 *   Ch 0  RADIO ADDRESS event → TIMER CAPTURE[2] task
 *   Ch 1  RADIO PHYEND event  → TIMER CAPTURE[4] task
 */
#define PGFSK_DPPI_CH_RX_TIMESTAMP      0U
#define PGFSK_DPPI_CH_PHYEND_TIMESTAMP  1U

/*
 * Timer CC channel assignments:
 *   CC[2]  RX ADDRESS timestamp
 *   CC[3]  deadline (generates semantic timeout event when reached)
 *   CC[4]  PHYEND timestamp
 *   CC[5]  ad-hoc "now" capture
 */
#define PGFSK_TIMER_CC_RX_TS            NRF_TIMER_CC_CHANNEL2
#define PGFSK_TIMER_CC_DEADLINE         NRF_TIMER_CC_CHANNEL3
#define PGFSK_TIMER_CC_PHYEND_TS        NRF_TIMER_CC_CHANNEL4
#define PGFSK_TIMER_CC_NOW              NRF_TIMER_CC_CHANNEL5

struct pgfsk_packet {
	uint8_t  length;
	uint16_t seq;
	uint8_t  data[PGFSK_PAYLOAD_MAX_LEN];
} __packed __aligned(4);

enum pgfsk_hw_event_type {
	PGFSK_HW_EVENT_RX_OK = 0,
	PGFSK_HW_EVENT_RX_BAD,
	PGFSK_HW_EVENT_TX_END,
	PGFSK_HW_EVENT_RX_INCOMPLETE,
	PGFSK_HW_EVENT_LISTEN_TIMEOUT,
	PGFSK_HW_EVENT_TX_TRIGGER_FAILED,
};

struct pgfsk_hw_event {
	enum pgfsk_hw_event_type type;
	uint32_t tick;
	int16_t rssi_dbm;
};

struct pgfsk_hw_stats {
	uint32_t packets_tx;
	uint32_t rx_ok_count;
	uint32_t crc_errors;
	uint32_t deadline_late_count;
	int16_t  last_rssi_dbm;
};

void pgfsk_hw_init(void);
void pgfsk_hw_start(void);
void pgfsk_hw_set_role_dongle(void);
void pgfsk_hw_set_role_headset(void);
void pgfsk_hw_stop(void);
void pgfsk_hw_get_stats(struct pgfsk_hw_stats *stats);

struct pgfsk_packet *pgfsk_hw_tx_get_wr_ptr(void);
void pgfsk_hw_tx_advance_wr_idx(void);
const struct pgfsk_packet *pgfsk_hw_rx_get_rd_ptr(void);
void pgfsk_hw_rx_advance_rd_idx(void);
bool pgfsk_hw_dequeue_event(struct pgfsk_hw_event *event, k_timeout_t timeout);
