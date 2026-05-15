#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <hal/nrf_dppi.h>
#include <hal/nrf_radio.h>
#include <hal/nrf_timer.h>
#include <zephyr/kernel.h>

/* Radio config */
#define PROP_RADIO_FREQUENCY_MHZ       2480
#define PROP_RADIO_TXPOWER             NRF_RADIO_TXPOWER_POS8DBM
#define PROP_RADIO_MODE_SETTING        NRF_RADIO_MODE_NRF_4MBIT_BT_0_6
#define PROP_RADIO_ADDR_DONGLE             0x55D391A5UL
#define PROP_RADIO_ADDR_HEADSET            0x55D391A5UL
#define PROP_RADIO_MAX_PACKET_AIRTIME_US   550U

/* Packet layout */
#define PROP_DATA_MAX_LEN              252
#define PROP_PACKET_METADATA_LEN       2U   /* seq(2) */
#define PROP_PACKET_MAX_LEN            (PROP_PACKET_METADATA_LEN + PROP_DATA_MAX_LEN)

#define PROP_KEEPALIVE_DATA_LEN        16U
#define PROP_KEEPALIVE_SEQ             UINT16_MAX
#define PROP_KEEPALIVE_LEN  (PROP_PACKET_METADATA_LEN + PROP_KEEPALIVE_DATA_LEN)

/*
 * Optional PHYEND scope probes. Enable by defining PROP_RADIO_DEBUG_PHYEND_GPIO.
 *
 * DEBUG CODE pins:
 *   P0.00: RADIO PHYEND DPPI/PPIB route through GPIOTE30 channel 3
 *   P0.01: firmware-observed RADIO PHYEND in the ISR
 */
#define PROP_RADIO_DEBUG_PHYEND_GPIO

/*
 * TIMER10 runs free at 1 MHz (prescaler 5 on a 32 MHz base clock).
 * 1 tick = 1 µs.  32-bit counter wraps after ~4295 s.
 */
#define PROP_TIMER                     NRF_TIMER10
#define PROP_TIMER_IRQn                TIMER10_IRQn
#define PROP_TIMER_FREQ_HZ             1000000U
#define PROP_TIMER_PRESCALER           5U
#define PROP_DPPI                      NRF_DPPIC10

/*
 * DPPI channel assignments (within DPPIC10):
 *   Ch 0  RADIO ADDRESS event → TIMER CAPTURE[2] task
 *   Ch 1  RADIO PHYEND event  → TIMER CAPTURE[4] task
 */
#define PROP_DPPI_CH_RX_TIMESTAMP      0U
#define PROP_DPPI_CH_PHYEND_TIMESTAMP  1U

/*
 * Timer CC channel assignments:
 *   CC[2]  RX ADDRESS timestamp
 *   CC[3]  deadline (generates semantic timeout event when reached)
 *   CC[4]  PHYEND timestamp
 *   CC[5]  ad-hoc "now" capture
 */
#define PROP_TIMER_CC_RX_TS            NRF_TIMER_CC_CHANNEL2
#define PROP_TIMER_CC_DEADLINE         NRF_TIMER_CC_CHANNEL3
#define PROP_TIMER_CC_PHYEND_TS        NRF_TIMER_CC_CHANNEL4
#define PROP_TIMER_CC_NOW              NRF_TIMER_CC_CHANNEL5

struct prop_packet {
	uint8_t  length;
	uint16_t seq;
	uint8_t  data[PROP_DATA_MAX_LEN];
} __packed __aligned(4);

enum prop_session_event_type {
	PROP_RADIO_SESSION_EVENT_RX_OK = 0,
	PROP_RADIO_SESSION_EVENT_RX_BAD,
	PROP_RADIO_SESSION_EVENT_TX_END,
	PROP_RADIO_SESSION_EVENT_RX_INCOMPLETE,
	PROP_RADIO_SESSION_EVENT_LISTEN_TIMEOUT,
	PROP_RADIO_SESSION_EVENT_TX_TRIGGER_FAILED,
};

struct prop_session_event {
	enum prop_session_event_type type;
	uint32_t tick;
	int16_t rssi_dbm;
};

struct prop_radio_stats {
	uint32_t packets_tx;
	uint32_t rx_ok_count;
	uint32_t crc_errors;
	uint32_t deadline_late_count;
	int16_t  last_rssi_dbm;
};

void prop_radio_init(void);
void prop_radio_start(void);
void prop_radio_set_role_dongle(void);
void prop_radio_set_role_headset(void);
void prop_radio_stop(void);
void prop_radio_get_stats(struct prop_radio_stats *s);

struct prop_packet *prop_radio_tx_get_wr_ptr(void);
void prop_radio_tx_advance_wr_idx(void);
const struct prop_packet *prop_radio_rx_get_rd_ptr(void);
void prop_radio_rx_advance_rd_idx(void);
bool prop_radio_dequeue_session_event(struct prop_session_event *event, k_timeout_t timeout);
