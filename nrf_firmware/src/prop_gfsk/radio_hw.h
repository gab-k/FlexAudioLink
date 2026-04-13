#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <hal/nrf_dppi.h>
#include <hal/nrf_radio.h>
#include <hal/nrf_timer.h>
#include <zephyr/kernel.h>

#include "app_control.h"

/* Radio config */
#define PROP_GFSK_RADIO_FREQUENCY_MHZ       2480
#define PROP_GFSK_RADIO_TXPOWER             NRF_RADIO_TXPOWER_POS8DBM
#define PROP_GFSK_RADIO_MODE_SETTING        NRF_RADIO_MODE_NRF_4MBIT_BT_0_6
#define PROP_GFSK_RADIO_ADDR_DONGLE         0x55D391A5UL
#define PROP_GFSK_RADIO_ADDR_HEADSET        0x55D391A5UL

/* Packet layout */
#define PROP_GFSK_PAYLOAD_LEN               242
#define PROP_GFSK_PACKET_METADATA_LEN       4U   /* seq(2) + payload_len(1) + reserved(1) */

/*
 * TIMER10 runs free at 1 MHz (prescaler 5 on a 32 MHz base clock).
 * 1 tick = 1 µs.  32-bit counter wraps after ~4295 s.
 */
#define PROP_GFSK_TIMER                     NRF_TIMER10
#define PROP_GFSK_TIMER_IRQn                TIMER10_IRQn
#define PROP_GFSK_TIMER_FREQ_HZ             1000000U
#define PROP_GFSK_TIMER_PRESCALER           5U
#define PROP_GFSK_DPPI                      NRF_DPPIC10

/*
 * DPPI channel assignments (within DPPIC10):
 *   Ch 0  TIMER CC[0] event  → RADIO TXEN task   (hardware TX launch)
 *   Ch 1  RADIO ADDRESS event → TIMER CAPTURE[2] task  (RX timestamp)
 *   Ch 2  RADIO PHYEND event  → TIMER CAPTURE[4] task
 *         (hardware captures every PHYEND; software currently reads it only
 *          for TX-done diagnostics)
 */
#define PROP_GFSK_DPPI_CH_TX_LAUNCH         0U
#define PROP_GFSK_DPPI_CH_RX_TIMESTAMP      1U
#define PROP_GFSK_DPPI_CH_PHYEND_TIMESTAMP  2U

/*
 * Timer CC channel assignments:
 *   CC[0]  TX deadline    – published on Ch 0 → RADIO TXEN
 *   CC[1]  Pre-TX window  – fires interrupt PRE_TX_OFFSET_US before CC[0]
 *   CC[2]  RX timestamp   – captured on RADIO ADDRESS via DPPI
 *   CC[4]  PHYEND timestamp – captured on RADIO PHYEND via DPPI, currently
 *          consumed only for TX-done diagnostics
 *   CC[5]  Now timestamp  – ad-hoc software capture for "current tick"
 */
#define PROP_GFSK_TIMER_CC_TX               NRF_TIMER_CC_CHANNEL0
#define PROP_GFSK_TIMER_CC_PRE_TX           NRF_TIMER_CC_CHANNEL1
#define PROP_GFSK_TIMER_CC_RX_TS            NRF_TIMER_CC_CHANNEL2
#define PROP_GFSK_TIMER_CC_PHYEND_TS        NRF_TIMER_CC_CHANNEL4
#define PROP_GFSK_TIMER_CC_NOW              NRF_TIMER_CC_CHANNEL5

/*
 * How far ahead of the TX deadline to abort ongoing RX and set up the TX
 * packet pointer.  This only needs to cover the deliberate DISABLE path plus
 * TX ramp-up; keeping it too large can truncate the peer's packet late in the
 * frame.
 */
#define PROP_GFSK_PRE_TX_OFFSET_US          150U

struct prop_gfsk_packet {
	uint8_t  length;
	uint16_t seq;
	uint8_t  payload_len;
	uint8_t  data[PROP_GFSK_PAYLOAD_LEN];
} __packed __aligned(4);

struct prop_gfsk_rx_frame {
	struct prop_gfsk_packet packet;
	int16_t  rssi_dbm;
	uint32_t rx_tick;   /* TIMER10 value captured on RADIO ADDRESS event */
};

struct prop_gfsk_hw_stats {
	uint32_t packets_tx;
	uint32_t packets_rx;
	uint32_t packets_lost;
	uint32_t crc_errors;
	uint32_t pretx_state_disabled_count;
	uint32_t pretx_state_rxidle_count;
	uint32_t pretx_state_rx_count;
	uint32_t pretx_state_rx_noaddr_count;
	uint32_t pretx_state_rx_addr_count;
	uint32_t pretx_state_other_count;
	int16_t  last_rssi_dbm;
};

void prop_gfsk_radio_hw_init(void);
void prop_gfsk_radio_hw_start(void);
void prop_gfsk_radio_hw_set_role(enum device_role role);
void prop_gfsk_radio_hw_stop(void);
void prop_gfsk_radio_hw_get_stats(struct prop_gfsk_hw_stats *stats);
void prop_gfsk_radio_hw_reset_stats(void);

/* Returns the current free-running TIMER10 tick (1 tick = 1 µs). */
uint32_t prop_gfsk_radio_hw_get_tick(void);

/*
 * Try to arm or update the next TX.  Returns false if the radio is already in
 * pre-TX/Tx handling or if tx_tick is too close to safely change.
 * If packet is non-NULL, it replaces the buffered TX packet for the pending
 * slot. If packet is NULL, the existing buffered packet is kept and only the
 * TX timing is updated.
 */
bool prop_gfsk_radio_hw_schedule_tx_if_possible(uint32_t tx_tick,
						const struct prop_gfsk_packet *packet);

/* Dequeue the next received frame.  Returns false on timeout. */
bool prop_gfsk_radio_hw_rx_dequeue(struct prop_gfsk_rx_frame *frame, k_timeout_t timeout);
struct k_msgq *prop_gfsk_radio_hw_rx_msgq(void);

/*
 * Wait for the most recently scheduled TX to complete.
 * Returns false on timeout (TX never fired or radio was stopped).
 */
bool prop_gfsk_radio_hw_tx_done_wait(k_timeout_t timeout);

/*
 * Returns a pointer to the TX-done semaphore for use with k_poll.
 * The semaphore is given once each time a TX completes.
 */
struct k_sem *prop_gfsk_radio_hw_tx_done_sem(void);

/* Returns the last hardware-captured TX PHYEND tick. */
uint32_t prop_gfsk_radio_hw_get_last_tx_done_tick(void);
