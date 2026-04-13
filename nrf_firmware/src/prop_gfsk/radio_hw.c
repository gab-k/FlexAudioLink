#include "prop_gfsk/radio_hw.h"

#include <string.h>

#include <hal/nrf_clock.h>
#include <soc.h>
#include <zephyr/sys/printk.h>

#define PROP_GFSK_RX_QUEUE_DEPTH 4

/* RX packet buffer written by the radio peripheral during reception. */
static struct prop_gfsk_packet g_rx_packet;

/* TX packet written by schedule_tx(), pointed to by the radio during TX. */
static struct prop_gfsk_packet g_tx_packet;

K_MSGQ_DEFINE(g_rx_queue, sizeof(struct prop_gfsk_rx_frame), PROP_GFSK_RX_QUEUE_DEPTH, 4);
K_SEM_DEFINE(g_tx_done_sem, 0, 1);

/*
 * g_in_tx_phase: set by the pre-TX timer ISR when it aborts RX to prepare for
 * TX.  Cleared by the PHYEND handler when TX PHYEND fires.  The DISABLED
 * handler uses this to distinguish a pre-TX abort (do not re-enter RX; TXEN
 * via DPPI will fire) from a post-TX transition (re-enter RX, give sem).
 *
 * g_tx_phyend_seen: set when TX PHYEND fires (g_in_tx_phase cleared at the
 * same time).  Checked in the DISABLED handler to distinguish post-TX DISABLED
 * (give g_tx_done_sem) from post-RX DISABLED (do not give sem).  Cleared once
 * the sem is given.
 */
static volatile bool g_in_tx_phase;
static volatile bool g_tx_phyend_seen;
static volatile bool g_tx_scheduled;
static volatile uint32_t g_scheduled_tx_tick;
static volatile uint32_t g_last_tx_done_tick;
static volatile uint32_t g_last_rx_address_tick;
static volatile bool g_last_rx_address_valid;

static struct prop_gfsk_hw_stats g_stats;
static uint16_t g_last_rx_seq;
static struct k_spinlock g_lock;

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

static bool seq_gap_from_expected(uint16_t expected, uint16_t seq, uint16_t *gap)
{
	uint16_t delta = (uint16_t)(seq - expected);

	if (delta == 0U) {
		*gap = 0U;
		return true;
	}

	/*
	 * Treat the lower half of the modulo-16-bit sequence space as "forward".
	 * This naturally handles 0xFFFF -> 0 wrap while ignoring stale/backward
	 * packets that would otherwise look like huge loss bursts.
	 */
	if (delta < 0x8000U) {
		*gap = delta;
		return true;
	}

	return false;
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
	const uint32_t dongle_base = (PROP_GFSK_RADIO_ADDR_DONGLE & 0x00FFFFFFUL) << 8;
	const uint32_t headset_base = (PROP_GFSK_RADIO_ADDR_HEADSET & 0x00FFFFFFUL) << 8;
	const uint32_t prefix0 = 
								((PROP_GFSK_RADIO_ADDR_DONGLE >> 24) & 0xFFU) |
							 	(((PROP_GFSK_RADIO_ADDR_HEADSET >> 24) & 0xFFU) << 8);

	/*
	 * With BALEN=3, each 32-bit address word is interpreted as:
	 *   [prefix byte][base byte 2][base byte 1][base byte 0]
	 * Both boards load the same logical address table; the runtime role only
	 * chooses which logical slot is used for TX versus RX.
	 */
	nrf_radio_base0_set(NRF_RADIO, dongle_base);
	nrf_radio_base1_set(NRF_RADIO, headset_base);
	nrf_radio_prefix0_set(NRF_RADIO, prefix0);
}

static void radio_enter_rx(void)
{
	g_last_rx_address_valid = false;
	nrf_radio_packetptr_set(NRF_RADIO, (uint32_t *)&g_rx_packet);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CRCERROR);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CRCOK);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_PHYEND);
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RXEN);
}

static void record_rx_stats(uint16_t seq, int16_t rssi_dbm)
{
	k_spinlock_key_t key = k_spin_lock(&g_lock);

	if (g_last_rx_seq != 0U) {
		uint16_t expected = g_last_rx_seq + 1U;
		uint16_t gap;

		if (seq_gap_from_expected(expected, seq, &gap) && gap > 0U) {
			g_stats.packets_lost += gap;
		}
	}

	g_last_rx_seq = seq;
	g_stats.packets_rx++;
	g_stats.last_rssi_dbm = rssi_dbm;
	k_spin_unlock(&g_lock, key);
}

/* --------------------------------------------------------------------------
 * ISRs
 * -------------------------------------------------------------------------- */

static void radio_isr(const void *arg)
{
	ARG_UNUSED(arg);

	/*
	 * PHYEND: fires at the last bit of every packet (TX or RX), independent
	 * of CRC result.  The PHYEND→DISABLE short fires simultaneously, so the
	 * radio is already ramping down to DISABLED.
	 *
	 * For TX: clear g_in_tx_phase so the upcoming DISABLED handler knows to
	 * re-enter RX and give the TX-done semaphore.  Set g_tx_phyend_seen so
	 * the DISABLED handler can distinguish post-TX from post-RX.
	 * For RX: nothing to do here; CRCOK/CRCERROR carry the result.
	 */
	if (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_PHYEND)) {
		nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_PHYEND);

		if (g_in_tx_phase) {
			g_in_tx_phase = false;
			g_tx_phyend_seen = true;
			g_tx_scheduled = false;
			g_last_tx_done_tick = nrf_timer_cc_get(PROP_GFSK_TIMER, PROP_GFSK_TIMER_CC_PHYEND_TS);

			k_spinlock_key_t key = k_spin_lock(&g_lock);
			g_stats.packets_tx++;
			k_spin_unlock(&g_lock, key);
		}
	}

	if (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS)) {
		nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);

		if (!g_in_tx_phase) {
			g_last_rx_address_tick = nrf_timer_cc_get(PROP_GFSK_TIMER, PROP_GFSK_TIMER_CC_RX_TS);
			g_last_rx_address_valid = true;
		}
	}

	/*
	 * CRCOK: RX packet received with valid CRC.  Read RSSI and timestamp,
	 * copy the packet, and post to the queue.  The radio is currently
	 * ramping down (PHYEND→DISABLE short already fired); RXEN will be
	 * called from the DISABLED handler once the ramp-down completes.
	 */
	if (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_CRCOK)) {
		nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CRCOK);

		int16_t rssi = -(int16_t)nrf_radio_rssi_sample_get(NRF_RADIO);
		uint32_t rx_tick = nrf_timer_cc_get(PROP_GFSK_TIMER,
						     PROP_GFSK_TIMER_CC_RX_TS);

		struct prop_gfsk_rx_frame frame = {
			.packet   = g_rx_packet,
			.rssi_dbm = rssi,
			.rx_tick  = rx_tick,
		};

		record_rx_stats(frame.packet.seq, rssi);
		(void)k_msgq_put(&g_rx_queue, &frame, K_NO_WAIT);
	}

	/* CRCERROR: bad packet; clear event.  DISABLED handler re-enters RX. */
	if (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_CRCERROR)) {
		nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CRCERROR);

		k_spinlock_key_t key = k_spin_lock(&g_lock);
		g_stats.crc_errors++;
		k_spin_unlock(&g_lock, key);
	}

	/*
	 * DISABLED: fires after the radio completes any ramp-down (post-TX or
	 * post-RX via PHYEND→DISABLE short, or after a deliberate DISABLE task).
	 *
	 * Three cases:
	 *  1. g_in_tx_phase is true   → pre-TX RX abort.  Do NOT re-enter RX;
	 *      CC[0] will fire TXEN via DPPI at the scheduled TX deadline.
	 *  2. g_tx_phyend_seen is true → post-TX.  Re-enter RX and notify the
	 *      link thread that the TX completed.
	 *  3. Neither flag set        → post-RX (CRCOK or CRCERROR path).
	 *      Stay disabled; the current MAC expects at most one RX packet in
	 *      the slot and will re-arm RX from the next post-TX path.
	 */
	if (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_DISABLED)) {
		nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);

		if (g_in_tx_phase) {
			/* Pre-TX abort; TXEN fires via DPPI. */
		} else if (g_tx_phyend_seen) {
			g_tx_phyend_seen = false;
			radio_enter_rx();
			k_sem_give(&g_tx_done_sem);
		}
	}
}

static void timer_isr(const void *arg)
{
	nrf_radio_state_t radio_state;
	k_spinlock_key_t key;

	ARG_UNUSED(arg);

	if (!nrf_timer_event_check(PROP_GFSK_TIMER, NRF_TIMER_EVENT_COMPARE1)) {
		return;
	}
	nrf_timer_event_clear(PROP_GFSK_TIMER, NRF_TIMER_EVENT_COMPARE1);

	radio_state = nrf_radio_state_get(NRF_RADIO);
	key = k_spin_lock(&g_lock);
	switch (radio_state) {
	case NRF_RADIO_STATE_DISABLED:
		g_stats.pretx_state_disabled_count++;
		break;
	case NRF_RADIO_STATE_RXIDLE:
		g_stats.pretx_state_rxidle_count++;
		break;
	case NRF_RADIO_STATE_RX:
		g_stats.pretx_state_rx_count++;
		if (g_last_rx_address_valid) {
			g_stats.pretx_state_rx_addr_count++;
		} else {
			g_stats.pretx_state_rx_noaddr_count++;
		}
		break;
	default:
		g_stats.pretx_state_other_count++;
		break;
	}
	k_spin_unlock(&g_lock, key);

	/*
	 * PRE_TX_OFFSET_US before the TX deadline:
	 *  1. Point the radio at the pre-loaded TX packet.
	 *  2. Set g_in_tx_phase so the DISABLED ISR doesn't re-enter RX.
	 *  3. Abort any ongoing RX so the radio reaches DISABLED before CC[0]
	 *     fires TXEN via DPPI.
	 */
	nrf_radio_packetptr_set(NRF_RADIO, (uint32_t *)&g_tx_packet);
	g_in_tx_phase = true;
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void prop_gfsk_radio_hw_init(void)
{
	hfclk_start();

	/* --- Radio base config --- */
	nrf_radio_mode_set(NRF_RADIO, PROP_GFSK_RADIO_MODE_SETTING);
	nrf_radio_frequency_set(NRF_RADIO, PROP_GFSK_RADIO_FREQUENCY_MHZ);
	nrf_radio_txpower_set(NRF_RADIO, PROP_GFSK_RADIO_TXPOWER);

	nrf_radio_packet_conf_t pkt_conf = {
		.lflen    = 8,
		.s0len    = 0,
		.s1len    = 0,
		.s1incl   = false,
		.plen     = NRF_RADIO_PREAMBLE_LENGTH_16BIT,
		.crcinc   = false,
		.maxlen   = sizeof(struct prop_gfsk_packet) - sizeof(uint8_t),
		.statlen  = 0,
		.balen    = 3,
		.big_endian = false,
		.whiteen  = true,
	};
	nrf_radio_packet_configure(NRF_RADIO, &pkt_conf);
	nrf_radio_datawhiteiv_set(NRF_RADIO,
				  (PROP_GFSK_RADIO_FREQUENCY_MHZ - 2400) & 0x3F);
	nrf_radio_crc_configure(NRF_RADIO, 2, NRF_RADIO_CRC_ADDR_SKIP, 0x11021UL);
	radio_program_address_table();

	/* Shorts: ramp-up → start, end of packet → disable, address → RSSI */
	nrf_radio_shorts_set(NRF_RADIO,
			     NRF_RADIO_SHORT_READY_START_MASK |
			     NRF_RADIO_SHORT_PHYEND_DISABLE_MASK |
			     NRF_RADIO_SHORT_ADDRESS_RSSISTART_MASK);

	/* --- TIMER10: free-running at 1 MHz --- */
	nrf_timer_mode_set(PROP_GFSK_TIMER, NRF_TIMER_MODE_TIMER);
	nrf_timer_bit_width_set(PROP_GFSK_TIMER, NRF_TIMER_BIT_WIDTH_32);
	nrf_timer_prescaler_set(PROP_GFSK_TIMER, PROP_GFSK_TIMER_PRESCALER);
	nrf_timer_int_enable(PROP_GFSK_TIMER, NRF_TIMER_INT_COMPARE1_MASK);

	/* --- DPPI wiring --- */

	/* Ch 0: TIMER CC[0] event → RADIO TXEN */
	nrf_timer_publish_set(PROP_GFSK_TIMER,
			      NRF_TIMER_EVENT_COMPARE0,
			      PROP_GFSK_DPPI_CH_TX_LAUNCH);
	nrf_radio_subscribe_set(NRF_RADIO,
				NRF_RADIO_TASK_TXEN,
				PROP_GFSK_DPPI_CH_TX_LAUNCH);

	/* Ch 1: RADIO ADDRESS → TIMER CAPTURE[2] (RX timestamp) */
	nrf_radio_publish_set(NRF_RADIO,
			      NRF_RADIO_EVENT_ADDRESS,
			      PROP_GFSK_DPPI_CH_RX_TIMESTAMP);
	nrf_timer_subscribe_set(PROP_GFSK_TIMER,
				NRF_TIMER_TASK_CAPTURE2,
				PROP_GFSK_DPPI_CH_RX_TIMESTAMP);

	/* Ch 2: RADIO PHYEND → TIMER CAPTURE[4] (used only for TX-done timestamp) */
	nrf_radio_publish_set(NRF_RADIO,
			      NRF_RADIO_EVENT_PHYEND,
			      PROP_GFSK_DPPI_CH_PHYEND_TIMESTAMP);
	nrf_timer_subscribe_set(PROP_GFSK_TIMER,
				NRF_TIMER_TASK_CAPTURE4,
				PROP_GFSK_DPPI_CH_PHYEND_TIMESTAMP);

	/* --- IRQs --- */
	IRQ_CONNECT(RADIO_0_IRQn, 0, radio_isr, NULL, 0);
	irq_enable(RADIO_0_IRQn);
	IRQ_CONNECT(PROP_GFSK_TIMER_IRQn, 0, timer_isr, NULL, 0);
	irq_enable(PROP_GFSK_TIMER_IRQn);

	prop_gfsk_radio_hw_reset_stats();
}

void prop_gfsk_radio_hw_start(void)
{
	nrf_timer_event_clear(PROP_GFSK_TIMER, NRF_TIMER_EVENT_COMPARE0);
	nrf_timer_event_clear(PROP_GFSK_TIMER, NRF_TIMER_EVENT_COMPARE1);
	nrf_timer_task_trigger(PROP_GFSK_TIMER, NRF_TIMER_TASK_CLEAR);
	nrf_timer_task_trigger(PROP_GFSK_TIMER, NRF_TIMER_TASK_START);

	nrf_dppi_channels_enable(
                              PROP_GFSK_DPPI,
                              BIT(PROP_GFSK_DPPI_CH_TX_LAUNCH) |
                              BIT(PROP_GFSK_DPPI_CH_RX_TIMESTAMP) |
                              BIT(PROP_GFSK_DPPI_CH_PHYEND_TIMESTAMP));

	nrf_radio_shorts_set(NRF_RADIO,
			     NRF_RADIO_SHORT_READY_START_MASK |
			     NRF_RADIO_SHORT_PHYEND_DISABLE_MASK |
			     NRF_RADIO_SHORT_ADDRESS_RSSISTART_MASK);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CRCERROR);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CRCOK);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_PHYEND);
	nrf_radio_int_enable(NRF_RADIO,
			     NRF_RADIO_INT_ADDRESS_MASK |
			     NRF_RADIO_INT_DISABLED_MASK |
			     NRF_RADIO_INT_CRCERROR_MASK |
			     NRF_RADIO_INT_CRCOK_MASK |
			     NRF_RADIO_INT_PHYEND_MASK);

	g_in_tx_phase = false;
	g_tx_phyend_seen = false;
	g_tx_scheduled = false;
	g_scheduled_tx_tick = 0U;
	g_last_tx_done_tick = 0U;
	g_last_rx_address_tick = 0U;
	g_last_rx_address_valid = false;
	k_msgq_purge(&g_rx_queue);
	k_sem_reset(&g_tx_done_sem);
	radio_enter_rx();
}

void prop_gfsk_radio_hw_set_role(enum device_role role)
{
	if (role == DEVICE_ROLE_DONGLE) {
		nrf_radio_txaddress_set(NRF_RADIO, 0);
		nrf_radio_rxaddresses_set(NRF_RADIO, 1 << 1);
	} else {
		nrf_radio_txaddress_set(NRF_RADIO, 1);
		nrf_radio_rxaddresses_set(NRF_RADIO, 1 << 0);
	}
}

void prop_gfsk_radio_hw_stop(void)
{
	nrf_timer_task_trigger(PROP_GFSK_TIMER, NRF_TIMER_TASK_STOP);
	nrf_radio_int_disable(NRF_RADIO,
			      NRF_RADIO_INT_ADDRESS_MASK |
			      NRF_RADIO_INT_DISABLED_MASK |
			      NRF_RADIO_INT_CRCERROR_MASK |
			      NRF_RADIO_INT_CRCOK_MASK |
			      NRF_RADIO_INT_PHYEND_MASK);
	nrf_radio_shorts_set(NRF_RADIO, 0U);
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
	nrf_dppi_channels_disable(PROP_GFSK_DPPI,
				  BIT(PROP_GFSK_DPPI_CH_TX_LAUNCH) |
				  BIT(PROP_GFSK_DPPI_CH_RX_TIMESTAMP) |
				  BIT(PROP_GFSK_DPPI_CH_PHYEND_TIMESTAMP));
	k_msgq_purge(&g_rx_queue);
	k_sem_reset(&g_tx_done_sem);
	g_in_tx_phase = false;
	g_tx_phyend_seen = false;
	g_tx_scheduled = false;
	g_scheduled_tx_tick = 0U;
	g_last_tx_done_tick = 0U;
	g_last_rx_address_tick = 0U;
	g_last_rx_address_valid = false;
}

void prop_gfsk_radio_hw_get_stats(struct prop_gfsk_hw_stats *stats)
{
	if (stats == NULL) {
		return;
	}

	k_spinlock_key_t key = k_spin_lock(&g_lock);
	*stats = g_stats;
	k_spin_unlock(&g_lock, key);
}

void prop_gfsk_radio_hw_reset_stats(void)
{
	k_spinlock_key_t key = k_spin_lock(&g_lock);

	memset(&g_stats, 0, sizeof(g_stats));
	g_stats.last_rssi_dbm = -100;
	g_last_rx_seq = 0U;
	g_last_rx_address_tick = 0U;
	g_last_rx_address_valid = false;

	k_spin_unlock(&g_lock, key);
}

uint32_t prop_gfsk_radio_hw_get_tick(void)
{
	nrf_timer_task_trigger(PROP_GFSK_TIMER, NRF_TIMER_TASK_CAPTURE5);
	return nrf_timer_cc_get(PROP_GFSK_TIMER, PROP_GFSK_TIMER_CC_NOW);
}

bool prop_gfsk_radio_hw_schedule_tx_if_possible(uint32_t tx_tick,
						const struct prop_gfsk_packet *packet)
{
	uint32_t now_tick;
	uint32_t pre_tx_tick;
	unsigned int irq_key;

	pre_tx_tick = tx_tick - PROP_GFSK_PRE_TX_OFFSET_US;
	irq_key = irq_lock();

	now_tick = prop_gfsk_radio_hw_get_tick();
	if (g_in_tx_phase ||
	    (int32_t)(pre_tx_tick - now_tick) <= 0) {
		irq_unlock(irq_key);
		return false;
	}

	if (packet == NULL && !g_tx_scheduled) {
		irq_unlock(irq_key);
		return false;
	}

	if (g_tx_scheduled &&
	    (int32_t)(g_scheduled_tx_tick - now_tick) <=
		    (int32_t)PROP_GFSK_PRE_TX_OFFSET_US) {
		irq_unlock(irq_key);
		return false;
	}

	if (packet != NULL) {
		memcpy(&g_tx_packet, packet, sizeof(g_tx_packet));
	}

	now_tick = prop_gfsk_radio_hw_get_tick();
	if (g_in_tx_phase ||
	    (int32_t)(pre_tx_tick - now_tick) <= 0) {
		irq_unlock(irq_key);
		return false;
	}

	if (g_tx_scheduled &&
	    (int32_t)(g_scheduled_tx_tick - now_tick) <=
		    (int32_t)PROP_GFSK_PRE_TX_OFFSET_US) {
		irq_unlock(irq_key);
		return false;
	}

	/*
	 * CC[1] fires the pre-TX ISR (abort RX, point radio at TX packet).
	 * CC[0] fires TXEN via DPPI at the exact TX deadline.
	 * Write CC[1] first so there's no window where CC[0] < CC[1].
	 */
	nrf_timer_cc_set(PROP_GFSK_TIMER,
			 PROP_GFSK_TIMER_CC_PRE_TX,
			 pre_tx_tick);
	nrf_timer_cc_set(PROP_GFSK_TIMER, PROP_GFSK_TIMER_CC_TX, tx_tick);
	g_scheduled_tx_tick = tx_tick;
	g_tx_scheduled = true;
	irq_unlock(irq_key);
	return true;
}

bool prop_gfsk_radio_hw_rx_dequeue(struct prop_gfsk_rx_frame *frame,
					    k_timeout_t timeout)
{
	if (frame == NULL) {
		return false;
	}

	return k_msgq_get(&g_rx_queue, frame, timeout) == 0;
}

struct k_msgq *prop_gfsk_radio_hw_rx_msgq(void)
{
	return &g_rx_queue;
}

bool prop_gfsk_radio_hw_tx_done_wait(k_timeout_t timeout)
{
	return k_sem_take(&g_tx_done_sem, timeout) == 0;
}

struct k_sem *prop_gfsk_radio_hw_tx_done_sem(void)
{
	return &g_tx_done_sem;
}

uint32_t prop_gfsk_radio_hw_get_last_tx_done_tick(void)
{
	return g_last_tx_done_tick;
}
