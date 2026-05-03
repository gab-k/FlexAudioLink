#include "prop_fsk/radio_core.h"

#include <string.h>

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/logging/log.h>

#include <hal/nrf_clock.h>
#include <soc.h>

#ifdef PFSK_RADIO_DEBUG_PHYEND_GPIO
#include <hal/nrf_gpiote.h>
#include <hal/nrf_gpio.h>
#include <hal/nrf_ppib.h>
#endif

LOG_MODULE_REGISTER(pfsk_radio_core, CONFIG_LOG_DEFAULT_LEVEL);

#define PFSK_RADIO_SESSION_EVENT_QUEUE_DEPTH 8
#define PFSK_RADIO_TX_RING_DEPTH     8U
#define PFSK_RADIO_RX_RING_DEPTH     8
#define PFSK_RADIO_LISTEN_TIMEOUT_BASE_US    1000U
#define PFSK_RADIO_LISTEN_TIMEOUT_JITTER_US  1000U
#define PFSK_RADIO_BASE_SHORTS \
		(NRF_RADIO_SHORT_READY_START_MASK | \
	 	NRF_RADIO_SHORT_PHYEND_DISABLE_MASK | \
	 	NRF_RADIO_SHORT_ADDRESS_RSSISTART_MASK)

#ifdef PFSK_RADIO_DEBUG_PHYEND_GPIO
#define PFSK_RADIO_DEBUG_PHYEND_DPPI_PIN     NRF_GPIO_PIN_MAP(0U, 0U)
#define PFSK_RADIO_DEBUG_PHYEND_ISR_PIN    NRF_GPIO_PIN_MAP(0U, 1U)
#define PFSK_RADIO_DEBUG_PHYEND_GPIOTE     NRF_GPIOTE30
#define PFSK_RADIO_DEBUG_PHYEND_GPIOTE_CH  3U
#endif

BUILD_ASSERT(PFSK_RADIO_TX_RING_DEPTH > 1U, "TX ring must leave one slot unused");
BUILD_ASSERT(PFSK_RADIO_RX_RING_DEPTH > 1U, "RX ring must leave one slot unused");
BUILD_ASSERT(PFSK_RADIO_TX_RING_DEPTH <= UINT8_MAX, "TX ring indices are uint8_t");
BUILD_ASSERT(PFSK_RADIO_RX_RING_DEPTH <= UINT8_MAX, "RX ring indices are uint8_t");
BUILD_ASSERT(PFSK_KEEPALIVE_PAYLOAD_LEN <= PFSK_PAYLOAD_MAX_LEN, "keepalive doesn't fit packet");
BUILD_ASSERT(PFSK_PACKET_MAX_LEN <= UINT8_MAX, "packet length must fit LFLEN");
BUILD_ASSERT(PFSK_KEEPALIVE_LEN <= UINT8_MAX, "keepalive length must fit LFLEN");
BUILD_ASSERT(offsetof(struct pfsk_packet, seq) == 1U, "seq must follow length byte");
BUILD_ASSERT(offsetof(struct pfsk_packet, payload) == 1U + PFSK_PACKET_METADATA_LEN,
	     "payload offset inconsistent with METADATA_LEN");

static struct pfsk_packet tx_ring[PFSK_RADIO_TX_RING_DEPTH];
static struct pfsk_packet rx_ring[PFSK_RADIO_RX_RING_DEPTH];
static struct pfsk_packet keepalive_packet = {
	.length = PFSK_KEEPALIVE_LEN,
	.seq = PFSK_KEEPALIVE_SEQ,
};
static volatile uint8_t tx_wr_idx;
static volatile uint8_t tx_rd_idx;
static volatile uint8_t rx_wr_idx;
static volatile uint8_t rx_rd_idx;

K_MSGQ_DEFINE(session_event_queue, sizeof(struct pfsk_session_event), PFSK_RADIO_SESSION_EVENT_QUEUE_DEPTH, 4);

enum pfsk_radio_turn_state {
	PFSK_RADIO_TURN_DISABLED = 0,
	PFSK_RADIO_TURN_LISTEN,
	PFSK_RADIO_TURN_IN_RX,
	PFSK_RADIO_TURN_IN_TX,
};

struct radio_core_state {
	enum pfsk_radio_turn_state turn_state;
	bool deadline_armed;
	bool tx_packet_from_ring;
};

static volatile struct radio_core_state radio;
static struct pfsk_radio_stats stats;
static struct k_spinlock lock;

#define PFSK_RADIO_DEADLINE_MIN_LEAD_US 20U

static void reset_stats(void);

#ifdef PFSK_RADIO_DEBUG_PHYEND_GPIO
static void debug_phyend_gpio_route_init(void)
{
	/* DEBUG CODE: bridge RADIO-domain PHYEND DPPI to P0.00/GPIOTE30. */
	nrf_ppib_subscribe_set(NRF_PPIB11,
			       nrf_ppib_send_task_get(PFSK_DPPI_CH_PHYEND_TIMESTAMP),
			       PFSK_DPPI_CH_PHYEND_TIMESTAMP);
	nrf_ppib_publish_set(NRF_PPIB21,
			     nrf_ppib_receive_event_get(PFSK_DPPI_CH_PHYEND_TIMESTAMP),
			     PFSK_DPPI_CH_PHYEND_TIMESTAMP);
	nrf_ppib_subscribe_set(NRF_PPIB22,
			       nrf_ppib_send_task_get(PFSK_DPPI_CH_PHYEND_TIMESTAMP),
			       PFSK_DPPI_CH_PHYEND_TIMESTAMP);
	nrf_ppib_publish_set(NRF_PPIB30,
			     nrf_ppib_receive_event_get(PFSK_DPPI_CH_PHYEND_TIMESTAMP),
			     PFSK_DPPI_CH_PHYEND_TIMESTAMP);
}

static void debug_phyend_gpio_route_enable(void)
{
	nrf_dppi_channels_enable(NRF_DPPIC20, BIT(PFSK_DPPI_CH_PHYEND_TIMESTAMP));
	nrf_dppi_channels_enable(NRF_DPPIC30, BIT(PFSK_DPPI_CH_PHYEND_TIMESTAMP));
}

static void debug_phyend_gpio_route_disable(void)
{
	nrf_dppi_channels_disable(NRF_DPPIC20, BIT(PFSK_DPPI_CH_PHYEND_TIMESTAMP));
	nrf_dppi_channels_disable(NRF_DPPIC30, BIT(PFSK_DPPI_CH_PHYEND_TIMESTAMP));
}

static void debug_phyend_gpio_init(void)
{
	debug_phyend_gpio_route_init();
	nrf_gpio_cfg_output(PFSK_RADIO_DEBUG_PHYEND_DPPI_PIN);
	nrf_gpio_pin_clear(PFSK_RADIO_DEBUG_PHYEND_DPPI_PIN);
	nrf_gpiote_task_configure(PFSK_RADIO_DEBUG_PHYEND_GPIOTE,
				  PFSK_RADIO_DEBUG_PHYEND_GPIOTE_CH,
				  PFSK_RADIO_DEBUG_PHYEND_DPPI_PIN,
				  NRF_GPIOTE_POLARITY_TOGGLE,
				  NRF_GPIOTE_INITIAL_VALUE_LOW);
	nrf_gpiote_task_enable(PFSK_RADIO_DEBUG_PHYEND_GPIOTE,
			       PFSK_RADIO_DEBUG_PHYEND_GPIOTE_CH);
	nrf_gpiote_subscribe_set(
		PFSK_RADIO_DEBUG_PHYEND_GPIOTE,
		nrf_gpiote_out_task_get(PFSK_RADIO_DEBUG_PHYEND_GPIOTE_CH),
		PFSK_DPPI_CH_PHYEND_TIMESTAMP);

	nrf_gpio_cfg_output(PFSK_RADIO_DEBUG_PHYEND_ISR_PIN);
	nrf_gpio_pin_clear(PFSK_RADIO_DEBUG_PHYEND_ISR_PIN);
}

static void debug_phyend_gpio_reset(void)
{
	nrf_gpio_pin_clear(PFSK_RADIO_DEBUG_PHYEND_DPPI_PIN);
	nrf_gpiote_task_force(PFSK_RADIO_DEBUG_PHYEND_GPIOTE,
			      PFSK_RADIO_DEBUG_PHYEND_GPIOTE_CH,
			      NRF_GPIOTE_INITIAL_VALUE_LOW);

	nrf_gpio_pin_clear(PFSK_RADIO_DEBUG_PHYEND_ISR_PIN);
}

static void debug_phyend_isr_toggle(void)
{
	nrf_gpio_pin_toggle(PFSK_RADIO_DEBUG_PHYEND_ISR_PIN);
}
#else
static inline void debug_phyend_gpio_init(void)
{
}

static inline void debug_phyend_gpio_reset(void)
{
}

static inline void debug_phyend_gpio_route_enable(void)
{
}

static inline void debug_phyend_gpio_route_disable(void)
{
}

static inline void debug_phyend_isr_toggle(void)
{
}
#endif

static uint8_t next_rx_wr_idx(void)
{
	return (rx_wr_idx + 1U) % PFSK_RADIO_RX_RING_DEPTH;
}

static void advance_tx_rd_idx(void)
{
	tx_rd_idx = (tx_rd_idx + 1U) % PFSK_RADIO_TX_RING_DEPTH;
}

static void advance_rx_wr_idx(void)
{
	rx_wr_idx = next_rx_wr_idx();
}

static void advance_rx_rd_idx(void)
{
	rx_rd_idx = (rx_rd_idx + 1U) % PFSK_RADIO_RX_RING_DEPTH;
}

static bool tx_ring_empty(void)
{
	return tx_rd_idx == tx_wr_idx;
}

static bool rx_ring_write_would_fill(void)
{
	return next_rx_wr_idx() == rx_rd_idx;
}

static void program_rx_packetptr(void)
{
	nrf_radio_packetptr_set(NRF_RADIO, (uint32_t *)&rx_ring[rx_wr_idx]);
	nrf_radio_shorts_set(NRF_RADIO,
			     PFSK_RADIO_BASE_SHORTS | NRF_RADIO_SHORT_DISABLED_RXEN_MASK);
}

static void program_tx_packetptr(void)
{
	if (tx_ring_empty()) {
		nrf_radio_packetptr_set(NRF_RADIO, (uint32_t *)&keepalive_packet);
		radio.tx_packet_from_ring = false;
	} else {
		nrf_radio_packetptr_set(NRF_RADIO, (uint32_t *)&tx_ring[tx_rd_idx]);
		radio.tx_packet_from_ring = true;
	}

	nrf_radio_shorts_set(NRF_RADIO,
			     PFSK_RADIO_BASE_SHORTS | NRF_RADIO_SHORT_DISABLED_TXEN_MASK);
}

static uint32_t capture_timer_tick(void)
{
	nrf_timer_task_trigger(PFSK_TIMER, NRF_TIMER_TASK_CAPTURE5);
	return nrf_timer_cc_get(PFSK_TIMER, PFSK_TIMER_CC_NOW);
}

static bool queue_session_event(const struct pfsk_session_event *event)
{
	if (event == NULL) {
		return false;
	}

	if (k_msgq_put(&session_event_queue, event, K_NO_WAIT) != 0) {
		LOG_ERR("PFSK radio event queue full, dropping event type %d", event->type);
		return false;
	}

	return true;
}

static void clear_deadline(void)
{
	radio.deadline_armed = false;
	nrf_timer_int_disable(PFSK_TIMER, NRF_TIMER_INT_COMPARE3_MASK);
}

static void set_deadline(uint32_t deadline_tick)
{
	uint32_t now_tick;

	if (radio.turn_state == PFSK_RADIO_TURN_DISABLED) {
		return;
	}

	now_tick = capture_timer_tick();
	if ((int32_t)(deadline_tick - now_tick) <= (int32_t)PFSK_RADIO_DEADLINE_MIN_LEAD_US) {
		k_spinlock_key_t key = k_spin_lock(&lock);

		deadline_tick = now_tick + PFSK_RADIO_DEADLINE_MIN_LEAD_US;
		stats.deadline_late_count++;

		k_spin_unlock(&lock, key);
	}

	nrf_timer_cc_set(PFSK_TIMER, PFSK_TIMER_CC_DEADLINE, deadline_tick);
	nrf_timer_event_clear(PFSK_TIMER, NRF_TIMER_EVENT_COMPARE3);
	radio.deadline_armed = true;
	nrf_timer_int_enable(PFSK_TIMER, NRF_TIMER_INT_COMPARE3_MASK);
}

static bool trigger_prepared_tx(void)
{
	nrf_radio_state_t radio_state;

	if (radio.turn_state == PFSK_RADIO_TURN_DISABLED ||
	    radio.turn_state == PFSK_RADIO_TURN_IN_TX) {
		return false;
	}

	radio_state = nrf_radio_state_get(NRF_RADIO);
	if (radio_state != NRF_RADIO_STATE_RX && radio_state != NRF_RADIO_STATE_RXIDLE) {
		return false;
	}

	program_tx_packetptr();
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
	radio.turn_state = PFSK_RADIO_TURN_IN_TX;
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);

	return true;
}

static uint32_t listen_timeout_jitter_us(void)
{
	static uint32_t probe_prng_state;
	uint32_t x;

	if (PFSK_RADIO_LISTEN_TIMEOUT_JITTER_US == 0U) {
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
	return x % (PFSK_RADIO_LISTEN_TIMEOUT_JITTER_US + 1U);
}

static void arm_initial_listen_deadline(void)
{
	set_deadline(capture_timer_tick() + PFSK_RADIO_LISTEN_TIMEOUT_BASE_US + listen_timeout_jitter_us());
}

static void arm_post_tx_listen_deadline(uint32_t tx_phyend_tick)
{
	set_deadline(tx_phyend_tick + PFSK_RADIO_MAX_PACKET_AIRTIME_US + listen_timeout_jitter_us());
}

static void arm_rx_deadline(uint32_t address_tick)
{
	set_deadline(address_tick + PFSK_RADIO_MAX_PACKET_AIRTIME_US);
}

static void note_rx_ok(int16_t rssi_dbm)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	stats.rx_ok_count++;
	stats.last_rssi_dbm = rssi_dbm;

	k_spin_unlock(&lock, key);
}

static void note_rx_bad(int16_t rssi_dbm)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	stats.crc_errors++;
	stats.last_rssi_dbm = rssi_dbm;

	k_spin_unlock(&lock, key);
}

static void note_tx_end(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	stats.packets_tx++;

	k_spin_unlock(&lock, key);
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
	const uint32_t dongle_base = (PFSK_RADIO_ADDR_DONGLE & 0x00FFFFFFUL) << 8;
	const uint32_t headset_base = (PFSK_RADIO_ADDR_HEADSET & 0x00FFFFFFUL) << 8;
	const uint32_t prefix0 = ((PFSK_RADIO_ADDR_DONGLE >> 24) & 0xFFU) |
							 (((PFSK_RADIO_ADDR_HEADSET >> 24) & 0xFFU) << 8);

	nrf_radio_base0_set(NRF_RADIO, dongle_base);
	nrf_radio_base1_set(NRF_RADIO, headset_base);
	nrf_radio_prefix0_set(NRF_RADIO, prefix0);
}

static void timer_isr(const void *arg)
{
	struct pfsk_session_event event;
	uint32_t deadline_tick;

	ARG_UNUSED(arg);

	if (nrf_timer_event_check(PFSK_TIMER, NRF_TIMER_EVENT_COMPARE3)) {
		nrf_timer_event_clear(PFSK_TIMER, NRF_TIMER_EVENT_COMPARE3);

		if (radio.deadline_armed) {
			deadline_tick = nrf_timer_cc_get(PFSK_TIMER, PFSK_TIMER_CC_DEADLINE);
			clear_deadline();

			memset(&event, 0, sizeof(event));
			event.tick = deadline_tick;

			if (radio.turn_state == PFSK_RADIO_TURN_IN_RX) {
				event.type = PFSK_RADIO_SESSION_EVENT_RX_INCOMPLETE;
				(void)queue_session_event(&event);
			} else if (radio.turn_state == PFSK_RADIO_TURN_LISTEN) {
				event.type = PFSK_RADIO_SESSION_EVENT_LISTEN_TIMEOUT;
				(void)queue_session_event(&event);
			} else {
				return;
			}

			if (!trigger_prepared_tx()) {
				event.type = PFSK_RADIO_SESSION_EVENT_TX_TRIGGER_FAILED;
				(void)queue_session_event(&event);
			}
		}
	}
}

static void radio_isr(const void *arg)
{
	ARG_UNUSED(arg);

	if (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS)) {
		nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);

		if (radio.turn_state == PFSK_RADIO_TURN_LISTEN) {
			uint32_t rx_tick = nrf_timer_cc_get(PFSK_TIMER, PFSK_TIMER_CC_RX_TS);

			radio.turn_state = PFSK_RADIO_TURN_IN_RX;
			arm_rx_deadline(rx_tick);
			program_tx_packetptr();
		} else if (radio.turn_state == PFSK_RADIO_TURN_IN_TX) {
			/* TX is already on air here. Switch back to RX before TX PHYEND,
			 * because the DISABLED short fires before the DISABLED ISR can run.
			 */
			program_rx_packetptr();
		}
	}

	if (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_CRCOK)) {
		struct pfsk_session_event event;
		int16_t rssi_dbm;

		nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CRCOK);

		if (radio.turn_state == PFSK_RADIO_TURN_LISTEN ||
		    radio.turn_state == PFSK_RADIO_TURN_IN_RX) {
			rssi_dbm = -(int16_t)nrf_radio_rssi_sample_get(NRF_RADIO);
			memset(&event, 0, sizeof(event));
			event.type = PFSK_RADIO_SESSION_EVENT_RX_OK;
			event.tick = nrf_timer_cc_get(PFSK_TIMER, PFSK_TIMER_CC_PHYEND_TS);
			event.rssi_dbm = rssi_dbm;
			note_rx_ok(rssi_dbm);
			clear_deadline();

			if (rx_ring_write_would_fill()) {
				LOG_ERR("PFSK RX ring full, dropping RX_OK");
			} else if (queue_session_event(&event)) {
				advance_rx_wr_idx();
			}
		}
	}

	if (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_CRCERROR)) {
		struct pfsk_session_event event;
		int16_t rssi_dbm;

		nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CRCERROR);

		if (radio.turn_state == PFSK_RADIO_TURN_LISTEN ||
		    radio.turn_state == PFSK_RADIO_TURN_IN_RX) {
			rssi_dbm = -(int16_t)nrf_radio_rssi_sample_get(NRF_RADIO);
			memset(&event, 0, sizeof(event));
			event.type = PFSK_RADIO_SESSION_EVENT_RX_BAD;
			event.tick = nrf_timer_cc_get(PFSK_TIMER, PFSK_TIMER_CC_PHYEND_TS);
			event.rssi_dbm = rssi_dbm;
			note_rx_bad(rssi_dbm);
			clear_deadline();

			if (rx_ring_write_would_fill()) {
				LOG_ERR("PFSK RX ring full, dropping RX_BAD");
			} else if (queue_session_event(&event)) {
				advance_rx_wr_idx();
			}
		}
	}

	if (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_PHYEND)) {
		nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_PHYEND);
		debug_phyend_isr_toggle();

		if (radio.turn_state == PFSK_RADIO_TURN_IN_RX) {
			radio.turn_state = PFSK_RADIO_TURN_IN_TX;
		} else if (radio.turn_state == PFSK_RADIO_TURN_IN_TX) {
			uint32_t tx_phyend_tick = nrf_timer_cc_get(PFSK_TIMER, PFSK_TIMER_CC_PHYEND_TS);
			struct pfsk_session_event event;

			if (radio.tx_packet_from_ring) {
				advance_tx_rd_idx();
			}

			radio.turn_state = PFSK_RADIO_TURN_LISTEN;
			arm_post_tx_listen_deadline(tx_phyend_tick);
			note_tx_end();

			memset(&event, 0, sizeof(event));
			event.type = PFSK_RADIO_SESSION_EVENT_TX_END;
			event.tick = tx_phyend_tick;
			(void)queue_session_event(&event);
		}
	}
}

void pfsk_radio_init(void)
{
	nrf_radio_packet_conf_t pkt_conf = {
		.lflen = 8,
		.s0len = 0,
		.s1len = 0,
		.s1incl = false,
		.plen = NRF_RADIO_PREAMBLE_LENGTH_16BIT,
		.crcinc = false,
		.maxlen = sizeof(struct pfsk_packet) - sizeof(uint8_t),
		.statlen = 0,
		.balen = 3,
		.big_endian = false,
		.whiteen = true,
	};

	hfclk_start();

	nrf_radio_mode_set(NRF_RADIO, PFSK_RADIO_MODE_SETTING);
	nrf_radio_frequency_set(NRF_RADIO, PFSK_RADIO_FREQUENCY_MHZ);
	nrf_radio_txpower_set(NRF_RADIO, PFSK_RADIO_TXPOWER);
	nrf_radio_packet_configure(NRF_RADIO, &pkt_conf);
	nrf_radio_datawhiteiv_set(NRF_RADIO, (PFSK_RADIO_FREQUENCY_MHZ - 2400) & 0x3F);
	nrf_radio_crc_configure(NRF_RADIO, 2, NRF_RADIO_CRC_ADDR_SKIP, 0x11021UL);
	radio_program_address_table();

	nrf_timer_mode_set(PFSK_TIMER, NRF_TIMER_MODE_TIMER);
	nrf_timer_bit_width_set(PFSK_TIMER, NRF_TIMER_BIT_WIDTH_32);
	nrf_timer_prescaler_set(PFSK_TIMER, PFSK_TIMER_PRESCALER);

	nrf_radio_publish_set(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS, PFSK_DPPI_CH_RX_TIMESTAMP);
	nrf_timer_subscribe_set(PFSK_TIMER, NRF_TIMER_TASK_CAPTURE2, PFSK_DPPI_CH_RX_TIMESTAMP);

	nrf_radio_publish_set(NRF_RADIO, NRF_RADIO_EVENT_PHYEND, PFSK_DPPI_CH_PHYEND_TIMESTAMP);
	nrf_timer_subscribe_set(PFSK_TIMER, NRF_TIMER_TASK_CAPTURE4, PFSK_DPPI_CH_PHYEND_TIMESTAMP);
	debug_phyend_gpio_init();

	IRQ_CONNECT(RADIO_0_IRQn, 0, radio_isr, NULL, 0);
	irq_enable(RADIO_0_IRQn);

	IRQ_CONNECT(TIMER10_IRQn, 1, timer_isr, NULL, 0);
	irq_enable(TIMER10_IRQn);

	reset_stats();
}

void pfsk_radio_start(void)
{
	unsigned int irq_key;

	k_msgq_purge(&session_event_queue);
	reset_stats();

	irq_key = irq_lock();

	memset((void *)&radio, 0, sizeof(radio));
	radio.turn_state = PFSK_RADIO_TURN_LISTEN;
	tx_wr_idx = 0U;
	tx_rd_idx = 0U;
	rx_wr_idx = 0U;
	rx_rd_idx = 0U;
	debug_phyend_gpio_reset();

	program_rx_packetptr();
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CRCERROR);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CRCOK);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_PHYEND);

	nrf_timer_task_trigger(PFSK_TIMER, NRF_TIMER_TASK_CLEAR);
	nrf_timer_task_trigger(PFSK_TIMER, NRF_TIMER_TASK_START);

	nrf_dppi_channels_enable(PFSK_DPPI,
				 BIT(PFSK_DPPI_CH_RX_TIMESTAMP) |
					 BIT(PFSK_DPPI_CH_PHYEND_TIMESTAMP));
	debug_phyend_gpio_route_enable();

	nrf_radio_int_enable(NRF_RADIO,
			     NRF_RADIO_INT_ADDRESS_MASK |
				     NRF_RADIO_INT_CRCERROR_MASK |
				     NRF_RADIO_INT_CRCOK_MASK |
				     NRF_RADIO_INT_PHYEND_MASK);

	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RXEN);
	arm_initial_listen_deadline();

	irq_unlock(irq_key);
}

void pfsk_radio_set_role_dongle(void)
{
	nrf_radio_txaddress_set(NRF_RADIO, 0);
	nrf_radio_rxaddresses_set(NRF_RADIO, 1 << 1);
}

void pfsk_radio_set_role_headset(void)
{
	nrf_radio_txaddress_set(NRF_RADIO, 1);
	nrf_radio_rxaddresses_set(NRF_RADIO, 1 << 0);
}

void pfsk_radio_stop(void)
{
	unsigned int irq_key;

	irq_key = irq_lock();

	memset((void *)&radio, 0, sizeof(radio));

	nrf_timer_task_trigger(PFSK_TIMER, NRF_TIMER_TASK_STOP);
	nrf_radio_int_disable(NRF_RADIO,
			      NRF_RADIO_INT_ADDRESS_MASK |
				      NRF_RADIO_INT_CRCERROR_MASK |
				      NRF_RADIO_INT_CRCOK_MASK |
				      NRF_RADIO_INT_PHYEND_MASK);
	nrf_radio_shorts_set(NRF_RADIO, 0U);
	nrf_dppi_channels_disable(
		PFSK_DPPI,
		BIT(PFSK_DPPI_CH_RX_TIMESTAMP) |
			BIT(PFSK_DPPI_CH_PHYEND_TIMESTAMP));
	debug_phyend_gpio_route_disable();
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CRCERROR);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CRCOK);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_PHYEND);

	irq_unlock(irq_key);

	k_msgq_purge(&session_event_queue);
}

void pfsk_radio_get_stats(struct pfsk_radio_stats *s)
{
	if (s == NULL) {
		return;
	}

	k_spinlock_key_t key = k_spin_lock(&lock);
	*s = stats;
	k_spin_unlock(&lock, key);
}

static void reset_stats(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	memset(&stats, 0, sizeof(stats));
	stats.last_rssi_dbm = -100;

	k_spin_unlock(&lock, key);
}

struct pfsk_packet *pfsk_radio_tx_get_wr_ptr(void)
{
	if (((tx_wr_idx + 1U) % PFSK_RADIO_TX_RING_DEPTH) == tx_rd_idx) {
		return NULL;
	}

	return &tx_ring[tx_wr_idx];
}

void pfsk_radio_tx_advance_wr_idx(void)
{
	tx_wr_idx = (tx_wr_idx + 1U) % PFSK_RADIO_TX_RING_DEPTH;
}

const struct pfsk_packet *pfsk_radio_rx_get_rd_ptr(void)
{
	if (radio.turn_state != PFSK_RADIO_TURN_DISABLED && rx_rd_idx != rx_wr_idx) {
		return &rx_ring[rx_rd_idx];
	}

	return NULL;
}

void pfsk_radio_rx_advance_rd_idx(void)
{
	if (rx_rd_idx != rx_wr_idx) {
		advance_rx_rd_idx();
	}
}

bool pfsk_radio_dequeue_session_event(struct pfsk_session_event *event, k_timeout_t timeout)
{
	if (event == NULL) {
		return false;
	}

	return k_msgq_get(&session_event_queue, event, timeout) == 0;
}
