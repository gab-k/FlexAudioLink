#include "prop_gfsk/radio_hw.h"

#include <string.h>

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/logging/log.h>

#include <hal/nrf_clock.h>
#include <soc.h>

LOG_MODULE_REGISTER(pgfsk_radio_hw, CONFIG_LOG_DEFAULT_LEVEL);

#define PGFSK_HW_EVENT_QUEUE_DEPTH 8
#define PGFSK_HW_TX_RING_DEPTH     8U
#define PGFSK_HW_RX_RING_DEPTH     8
#define PGFSK_HW_LISTEN_TIMEOUT_BASE_US    1000U
#define PGFSK_HW_LISTEN_TIMEOUT_JITTER_US  1000U
#define PGFSK_RADIO_BASE_SHORTS \
		(NRF_RADIO_SHORT_READY_START_MASK | \
	 	NRF_RADIO_SHORT_PHYEND_DISABLE_MASK | \
	 	NRF_RADIO_SHORT_ADDRESS_RSSISTART_MASK)

BUILD_ASSERT(PGFSK_HW_TX_RING_DEPTH > 1U, "TX ring must leave one slot unused");
BUILD_ASSERT(PGFSK_HW_RX_RING_DEPTH > 1U, "RX ring must leave one slot unused");
BUILD_ASSERT(PGFSK_HW_TX_RING_DEPTH <= UINT8_MAX, "TX ring indices are uint8_t");
BUILD_ASSERT(PGFSK_HW_RX_RING_DEPTH <= UINT8_MAX, "RX ring indices are uint8_t");
BUILD_ASSERT(offsetof(struct pgfsk_packet, seq) == 1U, "seq must follow length byte");
BUILD_ASSERT(offsetof(struct pgfsk_packet, data) == 1U + PGFSK_PACKET_METADATA_LEN,
	     "data offset inconsistent with METADATA_LEN");

static struct pgfsk_packet g_tx_ring[PGFSK_HW_TX_RING_DEPTH];
static struct pgfsk_packet g_rx_ring[PGFSK_HW_RX_RING_DEPTH];
static struct pgfsk_packet g_keepalive_packet = {
	.length = PGFSK_PACKET_METADATA_LEN,
};
static volatile uint8_t g_tx_wr_idx;
static volatile uint8_t g_tx_rd_idx;
static volatile uint8_t g_rx_wr_idx;
static volatile uint8_t g_rx_rd_idx;

K_MSGQ_DEFINE(g_radio_event_queue, sizeof(struct pgfsk_hw_event), PGFSK_HW_EVENT_QUEUE_DEPTH, 4);

enum pgfsk_hw_turn_state {
	PGFSK_HW_TURN_DISABLED = 0,
	PGFSK_HW_TURN_LISTEN,
	PGFSK_HW_TURN_IN_RX,
	PGFSK_HW_TURN_IN_TX,
};

struct radio_hw_state {
	enum pgfsk_hw_turn_state turn_state;
	bool deadline_armed;
	bool tx_packet_from_ring;
};

static volatile struct radio_hw_state g_hw;
static struct pgfsk_hw_stats g_stats;
static struct k_spinlock g_lock;

#define PGFSK_HW_DEADLINE_MIN_LEAD_US 20U

static void reset_stats(void);

static uint8_t next_rx_wr_idx(void)
{
	return (g_rx_wr_idx + 1U) % PGFSK_HW_RX_RING_DEPTH;
}

static void advance_tx_rd_idx(void)
{
	g_tx_rd_idx = (g_tx_rd_idx + 1U) % PGFSK_HW_TX_RING_DEPTH;
}

static void advance_rx_wr_idx(void)
{
	g_rx_wr_idx = next_rx_wr_idx();
}

static void advance_rx_rd_idx(void)
{
	g_rx_rd_idx = (g_rx_rd_idx + 1U) % PGFSK_HW_RX_RING_DEPTH;
}

static bool tx_ring_empty(void)
{
	return g_tx_rd_idx == g_tx_wr_idx;
}

static bool rx_ring_write_would_fill(void)
{
	return next_rx_wr_idx() == g_rx_rd_idx;
}

static void program_rx_packetptr(void)
{
	nrf_radio_packetptr_set(NRF_RADIO, (uint32_t *)&g_rx_ring[g_rx_wr_idx]);
	nrf_radio_shorts_set(NRF_RADIO,
			     PGFSK_RADIO_BASE_SHORTS | NRF_RADIO_SHORT_DISABLED_RXEN_MASK);
}

static void program_tx_packetptr(void)
{
	if (tx_ring_empty()) {
		nrf_radio_packetptr_set(NRF_RADIO, (uint32_t *)&g_keepalive_packet);
		g_hw.tx_packet_from_ring = false;
	} else {
		nrf_radio_packetptr_set(NRF_RADIO, (uint32_t *)&g_tx_ring[g_tx_rd_idx]);
		g_hw.tx_packet_from_ring = true;
	}

	nrf_radio_shorts_set(NRF_RADIO,
			     PGFSK_RADIO_BASE_SHORTS | NRF_RADIO_SHORT_DISABLED_TXEN_MASK);
}

static uint32_t capture_timer_tick(void)
{
	nrf_timer_task_trigger(PGFSK_TIMER, NRF_TIMER_TASK_CAPTURE5);
	return nrf_timer_cc_get(PGFSK_TIMER, PGFSK_TIMER_CC_NOW);
}

static bool queue_radio_event(const struct pgfsk_hw_event *event)
{
	if (event == NULL) {
		return false;
	}

	if (k_msgq_put(&g_radio_event_queue, event, K_NO_WAIT) != 0) {
		LOG_ERR("PGFSK HW event queue full, dropping event type %d", event->type);
		return false;
	}

	return true;
}

static void clear_deadline(void)
{
	g_hw.deadline_armed = false;
	nrf_timer_int_disable(PGFSK_TIMER, NRF_TIMER_INT_COMPARE3_MASK);
}

static void set_deadline(uint32_t deadline_tick)
{
	uint32_t now_tick;

	if (g_hw.turn_state == PGFSK_HW_TURN_DISABLED) {
		return;
	}

	now_tick = capture_timer_tick();
	if ((int32_t)(deadline_tick - now_tick) <= (int32_t)PGFSK_HW_DEADLINE_MIN_LEAD_US) {
		k_spinlock_key_t key = k_spin_lock(&g_lock);

		deadline_tick = now_tick + PGFSK_HW_DEADLINE_MIN_LEAD_US;
		g_stats.deadline_late_count++;

		k_spin_unlock(&g_lock, key);
	}

	nrf_timer_cc_set(PGFSK_TIMER, PGFSK_TIMER_CC_DEADLINE, deadline_tick);
	nrf_timer_event_clear(PGFSK_TIMER, NRF_TIMER_EVENT_COMPARE3);
	g_hw.deadline_armed = true;
	nrf_timer_int_enable(PGFSK_TIMER, NRF_TIMER_INT_COMPARE3_MASK);
}

static bool trigger_prepared_tx(void)
{
	nrf_radio_state_t radio_state;

	if (g_hw.turn_state == PGFSK_HW_TURN_DISABLED ||
	    g_hw.turn_state == PGFSK_HW_TURN_IN_TX) {
		return false;
	}

	radio_state = nrf_radio_state_get(NRF_RADIO);
	if (radio_state != NRF_RADIO_STATE_RX && radio_state != NRF_RADIO_STATE_RXIDLE) {
		return false;
	}

	program_tx_packetptr();
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
	g_hw.turn_state = PGFSK_HW_TURN_IN_TX;
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);

	return true;
}

static uint32_t listen_timeout_jitter_us(void)
{
	static uint32_t probe_prng_state;
	uint32_t x;

	if (PGFSK_HW_LISTEN_TIMEOUT_JITTER_US == 0U) {
		return 0U;
	}

	if (probe_prng_state == 0U) {
		uint8_t device_id[16];
		ssize_t len;

		probe_prng_state = 0x6d2b79f5U ^ k_cycle_get_32();
		len = hwinfo_get_device_id(device_id, sizeof(device_id));
		if (len > 0) {
			for (size_t i = 0; i < (size_t)len; ++i) {
				probe_prng_state ^= device_id[i];
				probe_prng_state *= 16777619U;
			}
		}

		if (probe_prng_state == 0U) {
			probe_prng_state = 0x1b873593U;
		}
	}

	x = probe_prng_state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	if (x == 0U) {
		x = 0x9e3779b9U;
	}

	probe_prng_state = x;
	return x % (PGFSK_HW_LISTEN_TIMEOUT_JITTER_US + 1U);
}

static void arm_initial_listen_deadline(void)
{
	set_deadline(capture_timer_tick() + PGFSK_HW_LISTEN_TIMEOUT_BASE_US + listen_timeout_jitter_us());
}

static void arm_post_tx_listen_deadline(uint32_t tx_phyend_tick)
{
	set_deadline(tx_phyend_tick + PGFSK_HW_MAX_PACKET_AIRTIME_US + listen_timeout_jitter_us());
}

static void arm_rx_deadline(uint32_t address_tick)
{
	set_deadline(address_tick + PGFSK_HW_MAX_PACKET_AIRTIME_US);
}

static void note_rx_ok(int16_t rssi_dbm)
{
	k_spinlock_key_t key = k_spin_lock(&g_lock);

	g_stats.rx_ok_count++;
	g_stats.last_rssi_dbm = rssi_dbm;

	k_spin_unlock(&g_lock, key);
}

static void note_rx_bad(int16_t rssi_dbm)
{
	k_spinlock_key_t key = k_spin_lock(&g_lock);

	g_stats.crc_errors++;
	g_stats.last_rssi_dbm = rssi_dbm;

	k_spin_unlock(&g_lock, key);
}

static void note_tx_end(void)
{
	k_spinlock_key_t key = k_spin_lock(&g_lock);

	g_stats.packets_tx++;

	k_spin_unlock(&g_lock, key);
}

static void hfclk_start(void)
{
	nrf_clock_event_clear(NRF_CLOCK, NRF_CLOCK_EVENT_HFCLKSTARTED);
	nrf_clock_task_trigger(NRF_CLOCK, NRF_CLOCK_TASK_HFCLKSTART);
	while (!nrf_clock_event_check(NRF_CLOCK, NRF_CLOCK_EVENT_HFCLKSTARTED)) {
	}
}

static void radio_program_address_table(void)
{
	const uint32_t dongle_base = (PGFSK_HW_ADDR_DONGLE & 0x00FFFFFFUL) << 8;
	const uint32_t headset_base = (PGFSK_HW_ADDR_HEADSET & 0x00FFFFFFUL) << 8;
	const uint32_t prefix0 = ((PGFSK_HW_ADDR_DONGLE >> 24) & 0xFFU) |
							 (((PGFSK_HW_ADDR_HEADSET >> 24) & 0xFFU) << 8);

	nrf_radio_base0_set(NRF_RADIO, dongle_base);
	nrf_radio_base1_set(NRF_RADIO, headset_base);
	nrf_radio_prefix0_set(NRF_RADIO, prefix0);
}

static void timer_isr(const void *arg)
{
	struct pgfsk_hw_event event;
	uint32_t deadline_tick;

	ARG_UNUSED(arg);

	if (nrf_timer_event_check(PGFSK_TIMER, NRF_TIMER_EVENT_COMPARE3)) {
		nrf_timer_event_clear(PGFSK_TIMER, NRF_TIMER_EVENT_COMPARE3);

		if (g_hw.deadline_armed) {
			deadline_tick = nrf_timer_cc_get(PGFSK_TIMER, PGFSK_TIMER_CC_DEADLINE);
			clear_deadline();

			memset(&event, 0, sizeof(event));
			event.tick = deadline_tick;

			if (g_hw.turn_state == PGFSK_HW_TURN_IN_RX) {
				event.type = PGFSK_HW_EVENT_RX_INCOMPLETE;
				(void)queue_radio_event(&event);
			} else if (g_hw.turn_state == PGFSK_HW_TURN_LISTEN) {
				event.type = PGFSK_HW_EVENT_LISTEN_TIMEOUT;
				(void)queue_radio_event(&event);
			} else {
				return;
			}

			if (!trigger_prepared_tx()) {
				event.type = PGFSK_HW_EVENT_TX_TRIGGER_FAILED;
				(void)queue_radio_event(&event);
			}
		}
	}
}

static void radio_isr(const void *arg)
{
	ARG_UNUSED(arg);

	if (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS)) {
		nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);

		if (g_hw.turn_state == PGFSK_HW_TURN_LISTEN) {
			uint32_t rx_tick = nrf_timer_cc_get(PGFSK_TIMER, PGFSK_TIMER_CC_RX_TS);

			g_hw.turn_state = PGFSK_HW_TURN_IN_RX;
			arm_rx_deadline(rx_tick);
			program_tx_packetptr();
		} else if (g_hw.turn_state == PGFSK_HW_TURN_IN_TX) {
			/* TX is already on air here. Switch back to RX before TX PHYEND,
			 * because the DISABLED short fires before the DISABLED ISR can run.
			 */
			program_rx_packetptr();
		}
	}

	if (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_CRCOK)) {
		struct pgfsk_hw_event event;
		int16_t rssi_dbm;

		nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CRCOK);

		if (g_hw.turn_state == PGFSK_HW_TURN_LISTEN ||
		    g_hw.turn_state == PGFSK_HW_TURN_IN_RX) {
			rssi_dbm = -(int16_t)nrf_radio_rssi_sample_get(NRF_RADIO);
			memset(&event, 0, sizeof(event));
			event.type = PGFSK_HW_EVENT_RX_OK;
			event.tick = nrf_timer_cc_get(PGFSK_TIMER, PGFSK_TIMER_CC_PHYEND_TS);
			event.rssi_dbm = rssi_dbm;
			note_rx_ok(rssi_dbm);
			clear_deadline();

			if (rx_ring_write_would_fill()) {
				LOG_ERR("PGFSK RX ring full, dropping RX_OK");
			} else if (queue_radio_event(&event)) {
				advance_rx_wr_idx();
			}
		}
	}

	if (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_CRCERROR)) {
		struct pgfsk_hw_event event;
		int16_t rssi_dbm;

		nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CRCERROR);

		if (g_hw.turn_state == PGFSK_HW_TURN_LISTEN ||
		    g_hw.turn_state == PGFSK_HW_TURN_IN_RX) {
			rssi_dbm = -(int16_t)nrf_radio_rssi_sample_get(NRF_RADIO);
			memset(&event, 0, sizeof(event));
			event.type = PGFSK_HW_EVENT_RX_BAD;
			event.tick = nrf_timer_cc_get(PGFSK_TIMER, PGFSK_TIMER_CC_PHYEND_TS);
			event.rssi_dbm = rssi_dbm;
			note_rx_bad(rssi_dbm);
			clear_deadline();

			if (rx_ring_write_would_fill()) {
				LOG_ERR("PGFSK RX ring full, dropping RX_BAD");
			} else if (queue_radio_event(&event)) {
				advance_rx_wr_idx();
			}
		}
	}

	if (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_PHYEND)) {
		nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_PHYEND);

		if (g_hw.turn_state == PGFSK_HW_TURN_IN_RX) {
			g_hw.turn_state = PGFSK_HW_TURN_IN_TX;
		} else if (g_hw.turn_state == PGFSK_HW_TURN_IN_TX) {
			uint32_t tx_phyend_tick = nrf_timer_cc_get(PGFSK_TIMER, PGFSK_TIMER_CC_PHYEND_TS);
			struct pgfsk_hw_event event;

			if (g_hw.tx_packet_from_ring) {
				advance_tx_rd_idx();
			}

			g_hw.turn_state = PGFSK_HW_TURN_LISTEN;
			arm_post_tx_listen_deadline(tx_phyend_tick);
			note_tx_end();

			memset(&event, 0, sizeof(event));
			event.type = PGFSK_HW_EVENT_TX_END;
			event.tick = tx_phyend_tick;
			(void)queue_radio_event(&event);
		}
	}
}

void pgfsk_hw_init(void)
{
	nrf_radio_packet_conf_t pkt_conf = {
		.lflen = 8,
		.s0len = 0,
		.s1len = 0,
		.s1incl = false,
		.plen = NRF_RADIO_PREAMBLE_LENGTH_16BIT,
		.crcinc = false,
		.maxlen = sizeof(struct pgfsk_packet) - sizeof(uint8_t),
		.statlen = 0,
		.balen = 3,
		.big_endian = false,
		.whiteen = true,
	};

	hfclk_start();

	nrf_radio_mode_set(NRF_RADIO, PGFSK_HW_MODE_SETTING);
	nrf_radio_frequency_set(NRF_RADIO, PGFSK_HW_FREQUENCY_MHZ);
	nrf_radio_txpower_set(NRF_RADIO, PGFSK_HW_TXPOWER);
	nrf_radio_packet_configure(NRF_RADIO, &pkt_conf);
	nrf_radio_datawhiteiv_set(NRF_RADIO, (PGFSK_HW_FREQUENCY_MHZ - 2400) & 0x3F);
	nrf_radio_crc_configure(NRF_RADIO, 2, NRF_RADIO_CRC_ADDR_SKIP, 0x11021UL);
	radio_program_address_table();

	nrf_timer_mode_set(PGFSK_TIMER, NRF_TIMER_MODE_TIMER);
	nrf_timer_bit_width_set(PGFSK_TIMER, NRF_TIMER_BIT_WIDTH_32);
	nrf_timer_prescaler_set(PGFSK_TIMER, PGFSK_TIMER_PRESCALER);

	nrf_radio_publish_set(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS, PGFSK_DPPI_CH_RX_TIMESTAMP);
	nrf_timer_subscribe_set(PGFSK_TIMER, NRF_TIMER_TASK_CAPTURE2, PGFSK_DPPI_CH_RX_TIMESTAMP);

	nrf_radio_publish_set(NRF_RADIO, NRF_RADIO_EVENT_PHYEND, PGFSK_DPPI_CH_PHYEND_TIMESTAMP);
	nrf_timer_subscribe_set(PGFSK_TIMER, NRF_TIMER_TASK_CAPTURE4, PGFSK_DPPI_CH_PHYEND_TIMESTAMP);

	IRQ_CONNECT(RADIO_0_IRQn, 0, radio_isr, NULL, 0);
	irq_enable(RADIO_0_IRQn);

	IRQ_CONNECT(TIMER10_IRQn, 1, timer_isr, NULL, 0);
	irq_enable(TIMER10_IRQn);

	reset_stats();
}

void pgfsk_hw_start(void)
{
	unsigned int irq_key;

	k_msgq_purge(&g_radio_event_queue);
	reset_stats();

	irq_key = irq_lock();

	memset((void *)&g_hw, 0, sizeof(g_hw));
	g_hw.turn_state = PGFSK_HW_TURN_LISTEN;
	g_tx_wr_idx = 0U;
	g_tx_rd_idx = 0U;
	g_rx_wr_idx = 0U;
	g_rx_rd_idx = 0U;

	program_rx_packetptr();
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CRCERROR);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CRCOK);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_PHYEND);

	nrf_timer_task_trigger(PGFSK_TIMER, NRF_TIMER_TASK_CLEAR);
	nrf_timer_task_trigger(PGFSK_TIMER, NRF_TIMER_TASK_START);

	nrf_dppi_channels_enable(PGFSK_DPPI,
							 BIT(PGFSK_DPPI_CH_RX_TIMESTAMP) | BIT(PGFSK_DPPI_CH_PHYEND_TIMESTAMP));

	nrf_radio_int_enable(NRF_RADIO,
			     NRF_RADIO_INT_ADDRESS_MASK |
				     NRF_RADIO_INT_CRCERROR_MASK |
				     NRF_RADIO_INT_CRCOK_MASK |
				     NRF_RADIO_INT_PHYEND_MASK);

	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RXEN);
	arm_initial_listen_deadline();

	irq_unlock(irq_key);
}

void pgfsk_hw_set_role_dongle(void)
{
	nrf_radio_txaddress_set(NRF_RADIO, 0);
	nrf_radio_rxaddresses_set(NRF_RADIO, 1 << 1);
}

void pgfsk_hw_set_role_headset(void)
{
	nrf_radio_txaddress_set(NRF_RADIO, 1);
	nrf_radio_rxaddresses_set(NRF_RADIO, 1 << 0);
}

void pgfsk_hw_stop(void)
{
	unsigned int irq_key;

	irq_key = irq_lock();

	memset((void *)&g_hw, 0, sizeof(g_hw));

	nrf_timer_task_trigger(PGFSK_TIMER, NRF_TIMER_TASK_STOP);
	nrf_radio_int_disable(NRF_RADIO,
			      NRF_RADIO_INT_ADDRESS_MASK |
				      NRF_RADIO_INT_CRCERROR_MASK |
				      NRF_RADIO_INT_CRCOK_MASK |
				      NRF_RADIO_INT_PHYEND_MASK);
	nrf_radio_shorts_set(NRF_RADIO, 0U);
	nrf_dppi_channels_disable(
		PGFSK_DPPI,
		BIT(PGFSK_DPPI_CH_RX_TIMESTAMP) |
			BIT(PGFSK_DPPI_CH_PHYEND_TIMESTAMP));
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CRCERROR);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CRCOK);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_PHYEND);

	irq_unlock(irq_key);

	k_msgq_purge(&g_radio_event_queue);
}

void pgfsk_hw_get_stats(struct pgfsk_hw_stats *stats)
{
	if (stats == NULL) {
		return;
	}

	k_spinlock_key_t key = k_spin_lock(&g_lock);
	*stats = g_stats;
	k_spin_unlock(&g_lock, key);
}

static void reset_stats(void)
{
	k_spinlock_key_t key = k_spin_lock(&g_lock);

	memset(&g_stats, 0, sizeof(g_stats));
	g_stats.last_rssi_dbm = -100;

	k_spin_unlock(&g_lock, key);
}

struct pgfsk_packet *pgfsk_hw_tx_get_wr_ptr(void)
{
	if (((g_tx_wr_idx + 1U) % PGFSK_HW_TX_RING_DEPTH) == g_tx_rd_idx) {
		return NULL;
	}

	return &g_tx_ring[g_tx_wr_idx];
}

void pgfsk_hw_tx_advance_wr_idx(void)
{
	g_tx_wr_idx = (g_tx_wr_idx + 1U) % PGFSK_HW_TX_RING_DEPTH;
}

const struct pgfsk_packet *pgfsk_hw_rx_get_rd_ptr(void)
{
	if (g_hw.turn_state != PGFSK_HW_TURN_DISABLED && g_rx_rd_idx != g_rx_wr_idx) {
		return &g_rx_ring[g_rx_rd_idx];
	}

	return NULL;
}

void pgfsk_hw_rx_advance_rd_idx(void)
{
	if (g_rx_rd_idx != g_rx_wr_idx) {
		advance_rx_rd_idx();
	}
}

bool pgfsk_hw_dequeue_event(struct pgfsk_hw_event *event, k_timeout_t timeout)
{
	if (event == NULL) {
		return false;
	}

	return k_msgq_get(&g_radio_event_queue, event, timeout) == 0;
}
