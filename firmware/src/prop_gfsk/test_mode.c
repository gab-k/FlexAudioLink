#include "prop_gfsk/test_mode.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app_control.h"
#include "prop_gfsk/link.h"

LOG_MODULE_REGISTER(pgfsk_test_mode, CONFIG_LOG_DEFAULT_LEVEL);

#define PGFSK_TEST_MODE_PAYLOAD_LEN        180U
#define PGFSK_TEST_TX_THREAD_STACK_SIZE    1536
#define PGFSK_TEST_RX_THREAD_STACK_SIZE    2048
#define PGFSK_TEST_THREAD_PRIORITY         6
#define PGFSK_TEST_EVENT_RUNNING           BIT(0)

static uint16_t g_test_tx_seq;
static size_t g_test_payload_len;
static bool g_test_mode_running;
static bool g_test_threads_started;
static K_THREAD_STACK_DEFINE(g_test_tx_thread_stack, PGFSK_TEST_TX_THREAD_STACK_SIZE);
static K_THREAD_STACK_DEFINE(g_test_rx_thread_stack, PGFSK_TEST_RX_THREAD_STACK_SIZE);
static struct k_thread g_test_tx_thread;
static struct k_thread g_test_rx_thread;
K_EVENT_DEFINE(g_test_events);

static void pgfsk_test_tx_thread(void *arg1, void *arg2, void *arg3);
static void pgfsk_test_rx_thread(void *arg1, void *arg2, void *arg3);

static void pgfsk_test_mode_ensure_threads(void)
{
	if (g_test_threads_started) {
		return;
	}

	k_thread_create(&g_test_tx_thread, g_test_tx_thread_stack,
			K_THREAD_STACK_SIZEOF(g_test_tx_thread_stack),
			pgfsk_test_tx_thread, NULL, NULL, NULL,
			PGFSK_TEST_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_create(&g_test_rx_thread, g_test_rx_thread_stack,
			K_THREAD_STACK_SIZEOF(g_test_rx_thread_stack),
			pgfsk_test_rx_thread, NULL, NULL, NULL,
			PGFSK_TEST_THREAD_PRIORITY, 0, K_NO_WAIT);
	g_test_threads_started = true;
}

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
	g_test_payload_len = PGFSK_PAYLOAD_MAX_LEN;
	k_event_clear(&g_test_events, PGFSK_TEST_EVENT_RUNNING);
}

static void pgfsk_test_start_traffic(size_t payload_len)
{
	if (payload_len == 0U || payload_len > PGFSK_PAYLOAD_MAX_LEN) {
		g_test_payload_len = PGFSK_PAYLOAD_MAX_LEN;
	} else {
		g_test_payload_len = payload_len;
	}
	k_event_set(&g_test_events, PGFSK_TEST_EVENT_RUNNING);
}

bool pgfsk_test_mode_stop(void)
{
	pgfsk_test_stop_traffic();
	g_test_mode_running = false;
	return true;
}

bool pgfsk_test_mode_start(void)
{
	if (pgfsk_test_mode_is_running()) {
		return true;
	}

	switch (app_control_get_current_profile()) {
	case APP_PROFILE_PGFSK_DONGLE:
	case APP_PROFILE_PGFSK_HEADSET:
		break;
	default:
		LOG_WRN("linktest requires a PGFSK profile");
		return false;
	}

	g_test_mode_running = true;
	pgfsk_test_mode_ensure_threads();
	pgfsk_test_start_traffic(PGFSK_TEST_MODE_PAYLOAD_LEN);
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
		if (!pgfsk_link_tx_enqueue(&frame, K_MSEC(10))) {
			k_sleep(K_MSEC(1));
		}
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
