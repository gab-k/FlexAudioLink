#include "proprietary/radio_hw.h"

#include <string.h>

#include <hal/nrf_clock.h>
#include <soc.h>

struct radio_packet pkt_tx;
struct radio_packet pkt_rx[2];
volatile uint8_t rx_buf_idx;

K_SEM_DEFINE(tx_done_sem, 1, 1);
K_SEM_DEFINE(rx_sem, 0, 1);
volatile int16_t rx_rssi;
volatile uint8_t rx_ready_idx;

static enum radio_runtime_mode g_runtime_mode = RADIO_RUNTIME_OFF;
static struct radio_stats g_radio_stats;
static uint16_t g_last_rx_seq;
static struct k_spinlock g_radio_lock;

static void hfclk_start(void)
{
	nrf_clock_event_clear(NRF_CLOCK, NRF_CLOCK_EVENT_HFCLKSTARTED);
	nrf_clock_task_trigger(NRF_CLOCK, NRF_CLOCK_TASK_HFCLKSTART);
	while (!nrf_clock_event_check(NRF_CLOCK, NRF_CLOCK_EVENT_HFCLKSTARTED)) {
	}
}

static void radio_stop_locked(void)
{
	nrf_radio_int_disable(NRF_RADIO,
			      NRF_RADIO_INT_DISABLED_MASK |
				      NRF_RADIO_INT_CRCERROR_MASK |
				      NRF_RADIO_INT_PHYEND_MASK);
	nrf_radio_shorts_set(NRF_RADIO, 0U);
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
}

static void radio_enter_tx_locked(void)
{
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
	nrf_radio_shorts_enable(NRF_RADIO,
				NRF_RADIO_SHORT_READY_START_MASK |
					NRF_RADIO_SHORT_PHYEND_DISABLE_MASK);
	nrf_radio_int_enable(NRF_RADIO, NRF_RADIO_INT_DISABLED_MASK);
	k_sem_reset(&tx_done_sem);
	k_sem_give(&tx_done_sem);
}

static void radio_enter_rx_locked(void)
{
	rx_buf_idx = 0U;
	k_sem_reset(&rx_sem);
	nrf_radio_packetptr_set(NRF_RADIO, (uint32_t *)&pkt_rx[0]);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CRCERROR);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_PHYEND);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_READY);
	nrf_radio_shorts_enable(NRF_RADIO,
				NRF_RADIO_SHORT_READY_START_MASK |
					NRF_RADIO_SHORT_ADDRESS_RSSISTART_MASK);
	nrf_radio_int_enable(NRF_RADIO,
			     NRF_RADIO_INT_CRCERROR_MASK |
				     NRF_RADIO_INT_PHYEND_MASK);
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RXEN);
}

static void radio_isr(const void *arg)
{
	ARG_UNUSED(arg);

	if (g_runtime_mode == RADIO_RUNTIME_TX) {
		if (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_DISABLED)) {
			nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
			k_sem_give(&tx_done_sem);
		}
		return;
	}

	if (g_runtime_mode != RADIO_RUNTIME_RX) {
		return;
	}

	if (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_CRCERROR)) {
		nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CRCERROR);
		nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_START);
		return;
	}

	if (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_PHYEND)) {
		nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_PHYEND);
		rx_rssi = nrf_radio_rssi_sample_get(NRF_RADIO);
		rx_ready_idx = rx_buf_idx;
		rx_buf_idx ^= 1U;
		nrf_radio_packetptr_set(NRF_RADIO, (uint32_t *)&pkt_rx[rx_buf_idx]);
		nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_START);
		k_sem_give(&rx_sem);
	}
}

void radio_hw_init(void)
{
	hfclk_start();

	nrf_radio_mode_set(NRF_RADIO, MODE);
	nrf_radio_frequency_set(NRF_RADIO, FREQ);
	nrf_radio_txpower_set(NRF_RADIO, TXPOWER);

	nrf_radio_packet_conf_t pkt_conf = {
		.lflen = 0,
		.s0len = 0,
		.s1len = 0,
		.s1incl = false,
		.plen = NRF_RADIO_PREAMBLE_LENGTH_8BIT,
		.crcinc = false,
		.maxlen = sizeof(struct radio_packet),
		.statlen = sizeof(struct radio_packet),
		.balen = 3,
		.big_endian = false,
		.whiteen = true,
	};

	nrf_radio_packet_configure(NRF_RADIO, &pkt_conf);
	nrf_radio_datawhiteiv_set(NRF_RADIO, (FREQ - 2400) & 0x3F);
	nrf_radio_crc_configure(NRF_RADIO, 2, NRF_RADIO_CRC_ADDR_SKIP, 0x11021UL);
	nrf_radio_base0_set(NRF_RADIO, SYNC_WORD);
	nrf_radio_txaddress_set(NRF_RADIO, 0);
	nrf_radio_rxaddresses_set(NRF_RADIO, 1 << 0);

	IRQ_CONNECT(RADIO_0_IRQn, 0, radio_isr, NULL, 0);
	irq_enable(RADIO_0_IRQn);

	radio_hw_reset_stats();
}

void radio_hw_set_runtime_mode(enum radio_runtime_mode mode)
{
	k_spinlock_key_t key = k_spin_lock(&g_radio_lock);

	if (g_runtime_mode == mode) {
		k_spin_unlock(&g_radio_lock, key);
		return;
	}

	radio_stop_locked();
	g_runtime_mode = mode;
	radio_hw_reset_stats();

	if (mode == RADIO_RUNTIME_TX) {
		radio_enter_tx_locked();
	} else if (mode == RADIO_RUNTIME_RX) {
		radio_enter_rx_locked();
	}

	k_spin_unlock(&g_radio_lock, key);
}

enum radio_runtime_mode radio_hw_get_runtime_mode(void)
{
	return g_runtime_mode;
}

void radio_hw_get_stats(struct radio_stats *stats)
{
	k_spinlock_key_t key;

	if (stats == NULL) {
		return;
	}

	key = k_spin_lock(&g_radio_lock);
	*stats = g_radio_stats;
	k_spin_unlock(&g_radio_lock, key);
}

void radio_hw_reset_stats(void)
{
	memset(&g_radio_stats, 0, sizeof(g_radio_stats));
	g_radio_stats.last_rssi_dbm = -100;
	g_radio_stats.peer_connected = (g_runtime_mode != RADIO_RUNTIME_OFF);
	g_last_rx_seq = 0U;
}

void radio_hw_record_tx(void)
{
	k_spinlock_key_t key = k_spin_lock(&g_radio_lock);

	g_radio_stats.packets_tx++;
	g_radio_stats.peer_connected = true;

	k_spin_unlock(&g_radio_lock, key);
}

void radio_hw_record_rx(uint16_t seq, int16_t rssi_dbm)
{
	k_spinlock_key_t key = k_spin_lock(&g_radio_lock);

	if (g_last_rx_seq != 0U) {
		uint16_t expected = g_last_rx_seq + 1U;

		if (seq != expected) {
			uint16_t gap = (uint16_t)(seq - expected);

			if (gap < 1000U) {
				g_radio_stats.packets_lost += gap;
			}
		}
	}

	g_last_rx_seq = seq;
	g_radio_stats.packets_rx++;
	g_radio_stats.last_rssi_dbm = rssi_dbm;
	g_radio_stats.peer_connected = true;

	k_spin_unlock(&g_radio_lock, key);
}
