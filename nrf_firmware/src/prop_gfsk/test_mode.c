#include "prop_gfsk/test_mode.h"

#include <string.h>

#include <zephyr/kernel.h>

#include "prop_gfsk/link.h"

#define PGFSK_TEST_MODE_PAYLOAD_LEN        180U
#define PGFSK_TEST_TX_THREAD_STACK_SIZE    1536
#define PGFSK_TEST_RX_THREAD_STACK_SIZE    2048
#define PGFSK_TEST_THREAD_PRIORITY         6
#define PGFSK_TEST_EVENT_RUNNING           BIT(0)

static uint16_t g_test_tx_seq;
static size_t g_test_payload_len;
static bool g_test_mode_running;
K_EVENT_DEFINE(g_test_events);

static void pgfsk_test_prepare_tx_frame(struct pgfsk_frame *frame)
{
	if (frame == NULL) {
		return;
	}

	*frame = (struct pgfsk_frame){
		.seq = g_test_tx_seq++,
		.len = g_test_payload_len,
		.rssi_dbm = 0,
	};
	memset(frame->payload, 0xAB, g_test_payload_len);
}

static void pgfsk_test_stop_traffic(void)
{
	g_test_tx_seq = 0U;
	g_test_payload_len = PGFSK_PAYLOAD_LEN;
	k_event_clear(&g_test_events, PGFSK_TEST_EVENT_RUNNING);
}

static void pgfsk_test_start_traffic(size_t payload_len)
{
	if (payload_len == 0U || payload_len > PGFSK_PAYLOAD_LEN) {
		g_test_payload_len = PGFSK_PAYLOAD_LEN;
	} else {
		g_test_payload_len = payload_len;
	}
	k_event_set(&g_test_events, PGFSK_TEST_EVENT_RUNNING);
}

bool pgfsk_test_mode_stop(void)
{
	pgfsk_test_stop_traffic();
	if (!pgfsk_link_stop()) {
		return false;
	}

	g_test_mode_running = false;
	return true;
}

bool pgfsk_test_mode_start(enum device_role local_device_role)
{
	struct pgfsk_link_config config = {
		.enabled = true,
		.local_device_role = local_device_role,
	};

	if (local_device_role != DEVICE_ROLE_DONGLE &&
	    local_device_role != DEVICE_ROLE_HEADSET) {
		return false;
	}

	if (app_control_get_current_operating_mode() != OPERATING_MODE_PROPRIETARY) {
		return false;
	}

	if (!pgfsk_link_set_config(&config)) {
		return false;
	}

	pgfsk_test_start_traffic(PGFSK_TEST_MODE_PAYLOAD_LEN);
	g_test_mode_running = true;
	return true;
}

bool pgfsk_test_mode_is_running(void)
{
	return g_test_mode_running;
}

static void pgfsk_test_tx_thread(void *arg1, void *arg2, void *arg3)
{
	struct pgfsk_frame frame;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (1) {
		while ((k_event_wait(&g_test_events, PGFSK_TEST_EVENT_RUNNING, false, K_FOREVER) &
				PGFSK_TEST_EVENT_RUNNING) == 0U) { }
		pgfsk_test_prepare_tx_frame(&frame);
		(void)pgfsk_link_tx_enqueue(&frame, K_FOREVER);
	}
}

static void pgfsk_test_rx_thread(void *arg1, void *arg2, void *arg3)
{
	struct pgfsk_frame frame;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (1) {
		while ((k_event_wait(&g_test_events, PGFSK_TEST_EVENT_RUNNING, false, K_FOREVER) &
				PGFSK_TEST_EVENT_RUNNING) == 0U) { }
		(void)pgfsk_link_rx_dequeue(&frame, K_MSEC(100));
	}
}

K_THREAD_DEFINE(pgfsk_test_tx_thread_id, PGFSK_TEST_TX_THREAD_STACK_SIZE,
				pgfsk_test_tx_thread, NULL, NULL, NULL,
				PGFSK_TEST_THREAD_PRIORITY, 0, 0);
K_THREAD_DEFINE(pgfsk_test_rx_thread_id, PGFSK_TEST_RX_THREAD_STACK_SIZE,
				pgfsk_test_rx_thread, NULL, NULL, NULL,
				PGFSK_TEST_THREAD_PRIORITY, 0, 0);
