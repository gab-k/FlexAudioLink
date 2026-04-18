#include "prop_gfsk/radio_hw.h"

#include <string.h>

#include <hal/nrf_clock.h>
#include <soc.h>

#define PGFSK_HW_EVENT_QUEUE_DEPTH 8
#define PGFSK_RADIO_BASE_SHORTS \
		(NRF_RADIO_SHORT_READY_START_MASK | \
	 	NRF_RADIO_SHORT_PHYEND_DISABLE_MASK | \
	 	NRF_RADIO_SHORT_ADDRESS_RSSISTART_MASK)

BUILD_ASSERT(offsetof(struct pgfsk_packet, seq) == 1U, "seq must follow length byte");
BUILD_ASSERT(offsetof(struct pgfsk_packet, data) == 1U + PGFSK_PACKET_METADATA_LEN,
	     "data offset inconsistent with METADATA_LEN");

static struct pgfsk_packet g_rx_packet;
static struct pgfsk_packet g_tx_packet;

K_MSGQ_DEFINE(g_radio_event_queue, sizeof(struct pgfsk_hw_event), PGFSK_HW_EVENT_QUEUE_DEPTH, 4);

struct radio_hw_state {
	bool running;
	bool in_tx_phase;
	bool tx_armed;
	bool tx_phyend_seen;
	bool deadline_armed;
	uint32_t last_tx_phyend_tick;
};

static volatile struct radio_hw_state g_hw;
static struct pgfsk_hw_stats g_stats;
static struct k_spinlock g_lock;

#define PGFSK_HW_RX_READY_POLL_US 100U
#define PGFSK_HW_DEADLINE_MIN_LEAD_US 20U

static void queue_radio_event(const struct pgfsk_hw_event *event)
{
	if (!g_hw.running || event == NULL) {
		return;
	}

	(void)k_msgq_put(&g_radio_event_queue, event, K_NO_WAIT);
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

static void radio_enter_rx(void)
{
	nrf_radio_packetptr_set(NRF_RADIO, (uint32_t *)&g_rx_packet);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CRCERROR);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CRCOK);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_PHYEND);
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RXEN);
}

static void timer_isr(const void *arg)
{
	struct pgfsk_hw_event event;

	ARG_UNUSED(arg);

	if (nrf_timer_event_check(PGFSK_TIMER, NRF_TIMER_EVENT_COMPARE3)) {
		nrf_timer_event_clear(PGFSK_TIMER, NRF_TIMER_EVENT_COMPARE3);

		if (g_hw.running && g_hw.deadline_armed) {
			g_hw.deadline_armed = false;
			nrf_timer_int_disable(PGFSK_TIMER, NRF_TIMER_INT_COMPARE3_MASK);

			memset(&event, 0, sizeof(event));
			event.type = PGFSK_HW_EVENT_TIMEOUT;
			event.tick = nrf_timer_cc_get(PGFSK_TIMER, PGFSK_TIMER_CC_DEADLINE);
			queue_radio_event(&event);
		}
	}
}

static void radio_isr(const void *arg)
{
	ARG_UNUSED(arg);

	if (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_PHYEND)) {
		nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_PHYEND);

		if (g_hw.running && g_hw.in_tx_phase) {
			g_hw.in_tx_phase = false;
			g_hw.tx_phyend_seen = true;
			g_hw.last_tx_phyend_tick = nrf_timer_cc_get(PGFSK_TIMER, PGFSK_TIMER_CC_PHYEND_TS);
			note_tx_end();
		}
	}

	if (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS)) {
		struct pgfsk_hw_event event;

		nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);

		if (g_hw.running && !g_hw.in_tx_phase) {
			uint32_t rx_tick = nrf_timer_cc_get(PGFSK_TIMER, PGFSK_TIMER_CC_RX_TS);

			memset(&event, 0, sizeof(event));
			event.type = PGFSK_HW_EVENT_RX_ADDRESS;
			event.tick = rx_tick;
			queue_radio_event(&event);
		} else if (g_hw.running && g_hw.in_tx_phase) {
			/* TX is already on air here, so it is safe to preload next RX buffer/shorts. */
			nrf_radio_packetptr_set(NRF_RADIO, (uint32_t *)&g_rx_packet);
			nrf_radio_shorts_set(NRF_RADIO, PGFSK_RADIO_BASE_SHORTS | NRF_RADIO_SHORT_DISABLED_RXEN_MASK);
		}
	}

	if (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_CRCOK)) {
		struct pgfsk_hw_event event;
		int16_t rssi_dbm;

		nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CRCOK);

		if (g_hw.running) {
			rssi_dbm = -(int16_t)nrf_radio_rssi_sample_get(NRF_RADIO);
			memset(&event, 0, sizeof(event));
			event.type = PGFSK_HW_EVENT_RX_OK;
			event.tick = nrf_timer_cc_get(PGFSK_TIMER, PGFSK_TIMER_CC_PHYEND_TS);
			event.packet = g_rx_packet;
			event.rssi_dbm = rssi_dbm;
			note_rx_ok(rssi_dbm);
			queue_radio_event(&event);
		}
	}

	if (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_CRCERROR)) {
		struct pgfsk_hw_event event;
		int16_t rssi_dbm;

		nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CRCERROR);

		if (g_hw.running) {
			rssi_dbm = -(int16_t)nrf_radio_rssi_sample_get(NRF_RADIO);
			memset(&event, 0, sizeof(event));
			event.type = PGFSK_HW_EVENT_RX_BAD;
			event.tick = nrf_timer_cc_get(PGFSK_TIMER, PGFSK_TIMER_CC_PHYEND_TS);
			event.rssi_dbm = rssi_dbm;
			note_rx_bad(rssi_dbm);
			queue_radio_event(&event);
		}
	}

	if (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_DISABLED)) {
		struct pgfsk_hw_event event;

		nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);

		if (!g_hw.running) {
			return;
		}

		if (g_hw.tx_armed) {
			g_hw.tx_armed = false;
			g_hw.in_tx_phase = true;
			return;
		}

		if (!g_hw.tx_phyend_seen) {
			return;
		}

		g_hw.tx_phyend_seen = false;

		memset(&event, 0, sizeof(event));
		event.type = PGFSK_HW_EVENT_TX_END;
		event.tick = g_hw.last_tx_phyend_tick;
		queue_radio_event(&event);
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

	pgfsk_hw_reset_stats();
}

void pgfsk_hw_start(void)
{
	unsigned int irq_key;

	k_msgq_purge(&g_radio_event_queue);

	irq_key = irq_lock();

	memset((void *)&g_hw, 0, sizeof(g_hw));

	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CRCERROR);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CRCOK);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_PHYEND);

	nrf_timer_task_trigger(PGFSK_TIMER, NRF_TIMER_TASK_CLEAR);
	nrf_timer_task_trigger(PGFSK_TIMER, NRF_TIMER_TASK_START);

	nrf_dppi_channels_enable(PGFSK_DPPI,
							 BIT(PGFSK_DPPI_CH_RX_TIMESTAMP) | BIT(PGFSK_DPPI_CH_PHYEND_TIMESTAMP));

	nrf_radio_shorts_set(NRF_RADIO,
			     PGFSK_RADIO_BASE_SHORTS |
				     NRF_RADIO_SHORT_DISABLED_RXEN_MASK);

	g_hw.running = true;

	nrf_radio_int_enable(NRF_RADIO,
			     NRF_RADIO_INT_ADDRESS_MASK |
				     NRF_RADIO_INT_DISABLED_MASK |
				     NRF_RADIO_INT_CRCERROR_MASK |
				     NRF_RADIO_INT_CRCOK_MASK |
				     NRF_RADIO_INT_PHYEND_MASK);

	irq_unlock(irq_key);
}

void pgfsk_hw_set_role(enum device_role role)
{
	if (role == DEVICE_ROLE_DONGLE) {
		nrf_radio_txaddress_set(NRF_RADIO, 0);
		nrf_radio_rxaddresses_set(NRF_RADIO, 1 << 1);
	} else {
		nrf_radio_txaddress_set(NRF_RADIO, 1);
		nrf_radio_rxaddresses_set(NRF_RADIO, 1 << 0);
	}
}

void pgfsk_hw_stop(void)
{
	unsigned int irq_key;

	irq_key = irq_lock();

	memset((void *)&g_hw, 0, sizeof(g_hw));

	nrf_timer_task_trigger(PGFSK_TIMER, NRF_TIMER_TASK_STOP);
	nrf_radio_int_disable(NRF_RADIO,
			      NRF_RADIO_INT_ADDRESS_MASK |
				      NRF_RADIO_INT_DISABLED_MASK |
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
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
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

void pgfsk_hw_reset_stats(void)
{
	k_spinlock_key_t key = k_spin_lock(&g_lock);

	memset(&g_stats, 0, sizeof(g_stats));
	g_stats.last_rssi_dbm = -100;

	k_spin_unlock(&g_lock, key);
}

uint32_t pgfsk_hw_get_tick(void)
{
	nrf_timer_task_trigger(PGFSK_TIMER, NRF_TIMER_TASK_CAPTURE5);
	return nrf_timer_cc_get(PGFSK_TIMER, PGFSK_TIMER_CC_NOW);
}

bool pgfsk_hw_wait_for_rx_active(void)
{
	for (uint32_t waited_us = 0U; waited_us < PGFSK_HW_RX_READY_POLL_US; ++waited_us) {
		if (nrf_radio_state_get(NRF_RADIO) == NRF_RADIO_STATE_RX) {
			return true;
		}
		k_busy_wait(1U);
	}
	return false;
}

bool pgfsk_hw_start_listen(void)
{
	nrf_radio_state_t radio_state;
	unsigned int irq_key;

	irq_key = irq_lock();

	if (!g_hw.running) {
		irq_unlock(irq_key);
		return false;
	}

	radio_state = nrf_radio_state_get(NRF_RADIO);
	if (radio_state == NRF_RADIO_STATE_RX || radio_state == NRF_RADIO_STATE_RXIDLE) {
		irq_unlock(irq_key);
		return true;
	}

	if (radio_state != NRF_RADIO_STATE_DISABLED) {
		irq_unlock(irq_key);
		return false;
	}

	radio_enter_rx();
	irq_unlock(irq_key);
	return true;
}

bool pgfsk_hw_prepare_tx(const struct pgfsk_packet *packet)
{
	unsigned int irq_key;

	if (packet == NULL) {
		return false;
	}

	irq_key = irq_lock();

	if (!g_hw.running || g_hw.in_tx_phase) {
		irq_unlock(irq_key);
		return false;
	}

	memcpy(&g_tx_packet, packet, sizeof(g_tx_packet));
	nrf_radio_packetptr_set(NRF_RADIO, (uint32_t *)&g_tx_packet);
	g_hw.tx_armed = true;
	nrf_radio_shorts_set(NRF_RADIO, PGFSK_RADIO_BASE_SHORTS | NRF_RADIO_SHORT_DISABLED_TXEN_MASK);

	irq_unlock(irq_key);
	return true;
}

bool pgfsk_hw_trigger_prepared_tx(void)
{
	nrf_radio_state_t radio_state;
	unsigned int irq_key;

	irq_key = irq_lock();

	if (!g_hw.running || g_hw.in_tx_phase || !g_hw.tx_armed) {
		irq_unlock(irq_key);
		return false;
	}

	radio_state = nrf_radio_state_get(NRF_RADIO);
	if (radio_state != NRF_RADIO_STATE_RX && radio_state != NRF_RADIO_STATE_RXIDLE) {
		irq_unlock(irq_key);
		return false;
	}

	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);

	irq_unlock(irq_key);
	return true;
}

void pgfsk_hw_set_deadline(uint32_t deadline_tick)
{
	unsigned int irq_key;
	uint32_t now_tick;

	irq_key = irq_lock();

	if (!g_hw.running) {
		irq_unlock(irq_key);
		return;
	}

	now_tick = pgfsk_hw_get_tick();
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

	irq_unlock(irq_key);
}

void pgfsk_hw_clear_deadline(void)
{
	unsigned int irq_key;

	irq_key = irq_lock();

	g_hw.deadline_armed = false;
	nrf_timer_int_disable(PGFSK_TIMER, NRF_TIMER_INT_COMPARE3_MASK);
	nrf_timer_event_clear(PGFSK_TIMER, NRF_TIMER_EVENT_COMPARE3);

	irq_unlock(irq_key);
}

bool pgfsk_hw_dequeue_event(struct pgfsk_hw_event *event, k_timeout_t timeout)
{
	if (event == NULL) {
		return false;
	}

	return k_msgq_get(&g_radio_event_queue, event, timeout) == 0;
}

struct k_msgq *pgfsk_hw_event_msgq(void)
{
	return &g_radio_event_queue;
}
