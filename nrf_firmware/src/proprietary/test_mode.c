#include "proprietary/test_mode.h"

#include <string.h>

#include <zephyr/sys/printk.h>

#include "proprietary/link.h"
#include "proprietary/radio_hw.h"

static bool g_test_mode_enabled;
static enum device_role g_test_role = DEVICE_ROLE_HEADSET;
static uint16_t tx_seq;
static bool tx_banner_printed;
static bool rx_banner_printed;

void proprietary_test_mode_init(void)
{
	g_test_mode_enabled = false;
	g_test_role = DEVICE_ROLE_HEADSET;
	tx_seq = 0U;
	tx_banner_printed = false;
	rx_banner_printed = false;
}

void proprietary_test_mode_set_enabled(bool enabled, enum device_role role)
{
	g_test_mode_enabled = enabled;
	g_test_role = role;
	tx_banner_printed = false;
	rx_banner_printed = false;

	if (!enabled) {
		proprietary_link_close();
		return;
	}

	(void)proprietary_link_open(&(struct proprietary_link_config) {
		.mode = (role == DEVICE_ROLE_DONGLE) ?
			PROPRIETARY_LINK_MODE_TX :
			PROPRIETARY_LINK_MODE_RX,
	});
}

bool proprietary_test_mode_is_enabled(void)
{
	return g_test_mode_enabled;
}

static void tx_thread(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (1) {
		/* Test mode is intentionally layered on top of the proprietary link API. */
		if (!g_test_mode_enabled ||
		    proprietary_link_get_mode() != PROPRIETARY_LINK_MODE_TX) {
			tx_banner_printed = false;
			k_sleep(K_MSEC(20));
			continue;
		}

		if (!tx_banner_printed) {
			printk("\n=== FlexLink Radio Test TX ===\n");
			printk("Frequency: %u MHz\n", FREQ);
			printk("Rate: 4 Mbps\n");
			printk("TX Power: +8 dBm\n");
			printk("Interval: %u ms\n", INTERVAL_MS);
			printk("Payload: %u bytes\n", PAYLOAD_LEN);
			printk("Sync word: 0x%08lX\n\n", SYNC_WORD);
			tx_banner_printed = true;
		}

		if (k_sem_take(&tx_done_sem, K_MSEC(20)) != 0) {
			continue;
		}

		if (!g_test_mode_enabled ||
		    proprietary_link_get_mode() != PROPRIETARY_LINK_MODE_TX) {
			continue;
		}

		nrf_radio_packetptr_set(NRF_RADIO, (uint32_t *)&pkt_tx);
		pkt_tx.seq = tx_seq++;
		memset(pkt_tx.data, 0xAB, sizeof(pkt_tx.data));
		nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
		nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_TXEN);
		radio_hw_record_tx();

		k_sleep(K_MSEC(INTERVAL_MS));
	}
}

static void rx_thread(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (1) {
		if (!g_test_mode_enabled ||
		    proprietary_link_get_mode() != PROPRIETARY_LINK_MODE_RX) {
			rx_banner_printed = false;
			k_sleep(K_MSEC(20));
			continue;
		}

		if (!rx_banner_printed) {
			printk("\n=== FlexLink Radio Test RX ===\n");
			printk("Frequency: %u MHz\n", FREQ);
			printk("Rate: 4 Mbps\n");
			printk("Payload: %u bytes\n", PAYLOAD_LEN);
			printk("Sync word: 0x%08lX\n\n", SYNC_WORD);
			rx_banner_printed = true;
		}

		if (k_sem_take(&rx_sem, K_MSEC(100)) == 0 &&
		    g_test_mode_enabled &&
		    proprietary_link_get_mode() == PROPRIETARY_LINK_MODE_RX) {
			uint8_t idx = rx_ready_idx;
			uint16_t seq = pkt_rx[idx].seq;
			int16_t rssi_dbm = -(int16_t)rx_rssi;

			radio_hw_record_rx(seq, rssi_dbm);
		}
	}
}

K_THREAD_DEFINE(proprietary_test_tx_thread_id, 1536, tx_thread,
		NULL, NULL, NULL, 6, 0, 0);
K_THREAD_DEFINE(proprietary_test_rx_thread_id, 2048, rx_thread,
		NULL, NULL, NULL, 6, 0, 0);
