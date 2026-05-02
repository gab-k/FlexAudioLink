#include "prop_fsk/test_mode.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app_control.h"
#include "prop_fsk/session.h"

LOG_MODULE_REGISTER(pfsk_test_mode, CONFIG_LOG_DEFAULT_LEVEL);

#define PFSK_TEST_MODE_PAYLOAD_LEN        180U
#define PFSK_TEST_TX_THREAD_STACK_SIZE    1536
#define PFSK_TEST_RX_THREAD_STACK_SIZE    2048
#define PFSK_TEST_THREAD_PRIORITY         6
#define PFSK_TEST_EVENT_RUNNING           BIT(0)

static uint8_t g_test_payload_len;
static bool g_test_mode_running;
static bool g_test_threads_started;
static K_THREAD_STACK_DEFINE(g_test_tx_thread_stack, PFSK_TEST_TX_THREAD_STACK_SIZE);
static K_THREAD_STACK_DEFINE(g_test_rx_thread_stack, PFSK_TEST_RX_THREAD_STACK_SIZE);
static struct k_thread g_test_tx_thread;
static struct k_thread g_test_rx_thread;
K_EVENT_DEFINE(g_test_events);

static void pfsk_test_tx_thread(void *arg1, void *arg2, void *arg3);
static void pfsk_test_rx_thread(void *arg1, void *arg2, void *arg3);

static void pfsk_test_mode_ensure_threads(void)
{
	if (g_test_threads_started) {
		return;
	}

	k_thread_create(&g_test_tx_thread, g_test_tx_thread_stack,
			K_THREAD_STACK_SIZEOF(g_test_tx_thread_stack),
			pfsk_test_tx_thread, NULL, NULL, NULL,
			PFSK_TEST_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_create(&g_test_rx_thread, g_test_rx_thread_stack,
			K_THREAD_STACK_SIZEOF(g_test_rx_thread_stack),
			pfsk_test_rx_thread, NULL, NULL, NULL,
			PFSK_TEST_THREAD_PRIORITY, 0, K_NO_WAIT);
	g_test_threads_started = true;
}

static void pfsk_test_prepare_tx_packet(struct pfsk_packet *packet)
{
	if (packet == NULL) {
		return;
	}

	*packet = (struct pfsk_packet){
		.length = PFSK_PACKET_METADATA_LEN + g_test_payload_len,
	};
	memset(packet->payload, 0xAB, g_test_payload_len);
}

static void pfsk_test_stop_traffic(void)
{
	g_test_payload_len = PFSK_PAYLOAD_MAX_LEN;
	k_event_clear(&g_test_events, PFSK_TEST_EVENT_RUNNING);
}

static void pfsk_test_start_traffic(size_t payload_len)
{
	if (payload_len == 0U || payload_len > PFSK_PAYLOAD_MAX_LEN) {
		g_test_payload_len = PFSK_PAYLOAD_MAX_LEN;
	} else {
		g_test_payload_len = payload_len;
	}
	k_event_set(&g_test_events, PFSK_TEST_EVENT_RUNNING);
}

bool pfsk_test_mode_stop(void)
{
	pfsk_test_stop_traffic();
	g_test_mode_running = false;
	return true;
}

bool pfsk_test_mode_start(void)
{
	if (pfsk_test_mode_is_running()) {
		return true;
	}

	switch (app_control_get_current_profile()) {
	case APP_PROFILE_PFSK_DONGLE:
	case APP_PROFILE_PFSK_HEADSET:
		break;
	default:
		LOG_WRN("linktest requires a PFSK profile");
		return false;
	}

	g_test_mode_running = true;
	pfsk_test_mode_ensure_threads();
	pfsk_test_start_traffic(PFSK_TEST_MODE_PAYLOAD_LEN);
	return true;
}

bool pfsk_test_mode_is_running(void)
{
	return g_test_mode_running;
}

static void pfsk_test_tx_thread(void *arg1, void *arg2, void *arg3)
{
	struct pfsk_packet packet;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (1) {
		while ((k_event_wait(&g_test_events, PFSK_TEST_EVENT_RUNNING, false, K_FOREVER) &
				PFSK_TEST_EVENT_RUNNING) == 0U) { }
		pfsk_test_prepare_tx_packet(&packet);
		if (!pfsk_session_tx_enqueue(&packet, K_MSEC(10))) {
			k_sleep(K_MSEC(1));
		}
	}
}

static void pfsk_test_rx_thread(void *arg1, void *arg2, void *arg3)
{
	struct pfsk_packet packet;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (1) {
		while ((k_event_wait(&g_test_events, PFSK_TEST_EVENT_RUNNING, false, K_FOREVER) &
				PFSK_TEST_EVENT_RUNNING) == 0U) { }
		(void)pfsk_session_rx_dequeue(&packet, K_MSEC(100));
	}
}
