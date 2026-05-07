#include "prop/test_mode.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app_control.h"
#include "prop/session.h"

LOG_MODULE_REGISTER(prop_test_mode, CONFIG_LOG_DEFAULT_LEVEL);

#define PROP_TEST_MODE_PAYLOAD_LEN        180U
#define PROP_TEST_TX_THREAD_STACK_SIZE    1536
#define PROP_TEST_RX_THREAD_STACK_SIZE    2048
#define PROP_TEST_THREAD_PRIORITY         6
#define PROP_TEST_EVENT_RUNNING           BIT(0)

static uint8_t test_payload_len;
static bool test_mode_running;
static bool test_threads_started;
static K_THREAD_STACK_DEFINE(test_tx_thread_stack, PROP_TEST_TX_THREAD_STACK_SIZE);
static K_THREAD_STACK_DEFINE(test_rx_thread_stack, PROP_TEST_RX_THREAD_STACK_SIZE);
static struct k_thread test_tx_thread;
static struct k_thread test_rx_thread;
K_EVENT_DEFINE(test_events);

static void prop_test_tx_thread(void *arg1, void *arg2, void *arg3);
static void prop_test_rx_thread(void *arg1, void *arg2, void *arg3);

static void prop_test_mode_ensure_threads(void)
{
	if (test_threads_started) {
		return;
	}

	k_thread_create(&test_tx_thread, test_tx_thread_stack,
			K_THREAD_STACK_SIZEOF(test_tx_thread_stack),
			prop_test_tx_thread, NULL, NULL, NULL,
			PROP_TEST_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_create(&test_rx_thread, test_rx_thread_stack,
			K_THREAD_STACK_SIZEOF(test_rx_thread_stack),
			prop_test_rx_thread, NULL, NULL, NULL,
			PROP_TEST_THREAD_PRIORITY, 0, K_NO_WAIT);
	test_threads_started = true;
}

static void prop_test_prepare_tx_packet(struct prop_packet *packet)
{
	if (packet == NULL) {
		return;
	}

	*packet = (struct prop_packet){
		.length = PROP_PACKET_METADATA_LEN + test_payload_len,
	};
	memset(packet->payload, 0xAB, test_payload_len);
}

static void prop_test_stop_traffic(void)
{
	test_payload_len = PROP_PAYLOAD_MAX_LEN;
	k_event_clear(&test_events, PROP_TEST_EVENT_RUNNING);
}

static void prop_test_start_traffic(size_t payload_len)
{
	if (payload_len == 0U || payload_len > PROP_PAYLOAD_MAX_LEN) {
		test_payload_len = PROP_PAYLOAD_MAX_LEN;
	} else {
		test_payload_len = payload_len;
	}
	k_event_set(&test_events, PROP_TEST_EVENT_RUNNING);
}

bool prop_test_mode_stop(void)
{
	prop_test_stop_traffic();
	test_mode_running = false;
	return true;
}

bool prop_test_mode_start(void)
{
	if (prop_test_mode_is_running()) {
		return true;
	}

	switch (app_control_get_current_mode()) {
	case APP_MODE_PROP_DONGLE:
	case APP_MODE_PROP_HEADSET:
		break;
	default:
		LOG_WRN("linktest requires a PROP mode");
		return false;
	}

	test_mode_running = true;
	prop_test_mode_ensure_threads();
	prop_test_start_traffic(PROP_TEST_MODE_PAYLOAD_LEN);
	return true;
}

bool prop_test_mode_is_running(void)
{
	return test_mode_running;
}

static void prop_test_tx_thread(void *arg1, void *arg2, void *arg3)
{
	struct prop_packet packet;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (1) {
		while ((k_event_wait(&test_events, PROP_TEST_EVENT_RUNNING, false, K_FOREVER) &
				PROP_TEST_EVENT_RUNNING) == 0U) { }
		prop_test_prepare_tx_packet(&packet);
		if (!prop_session_tx_enqueue(&packet, K_MSEC(10))) {
			k_sleep(K_MSEC(1));
		}
	}
}

static void prop_test_rx_thread(void *arg1, void *arg2, void *arg3)
{
	struct prop_packet packet;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (1) {
		while ((k_event_wait(&test_events, PROP_TEST_EVENT_RUNNING, false, K_FOREVER) &
				PROP_TEST_EVENT_RUNNING) == 0U) { }
		(void)prop_session_rx_dequeue(&packet, K_MSEC(100));
	}
}
