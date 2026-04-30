#include "prop_gfsk/link.h"

#include <string.h>

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(pgfsk_link, CONFIG_LOG_DEFAULT_LEVEL);

#define PGFSK_LINK_THREAD_STACK_SIZE       2048
#define PGFSK_LINK_THREAD_PRIORITY         6
#define PGFSK_LINK_QUEUE_DEPTH             4
#define PGFSK_LINK_MAX_PACKET_AIRTIME_US   550U
#define PGFSK_LINK_SYNC_LOSS_TURNS         8U
#define PGFSK_LINK_FIXED_TICK_US           1000U
#define PGFSK_LINK_RANDOM_TICKS_US         1000U
enum pgfsk_internal_state {
	PGFSK_STATE_IN_RX = 0,
	PGFSK_STATE_IN_TX,
	PGFSK_STATE_LISTEN,
};

struct link_stats {
	uint32_t packets_lost_in_service;
	uint32_t loss_burst_1_count;
	uint32_t loss_burst_2_count;
	uint32_t loss_burst_3_4_count;
	uint32_t loss_burst_5_plus_count;
	uint32_t max_loss_burst_len;
	uint32_t outage_count;
	uint32_t rx_incomplete_count;
	uint32_t tx_trigger_fail_count;
};

struct pgfsk_link_runtime {
	enum pgfsk_link_state service_state;
	enum pgfsk_internal_state state;
	struct link_stats stats;
	uint32_t rx_deadline_tick;
	uint8_t consecutive_rx_misses;
	uint64_t in_service_since_cyc;
	uint16_t next_tx_seq;
	uint16_t last_rx_seq;
	bool have_last_rx_seq;
	struct pgfsk_packet prepared_tx_packet;
	struct k_mutex mutex_lock;
};

static struct pgfsk_link_runtime g_link = {
	.service_state = PGFSK_LINK_STATE_NO_SERVICE,
	.state = PGFSK_STATE_LISTEN,
	.mutex_lock = Z_MUTEX_INITIALIZER(g_link.mutex_lock),
};

static uint32_t pgfsk_link_random_probe_jitter(void);
static void pgfsk_link_compose_packet(struct pgfsk_packet *packet, uint16_t seq);

K_MSGQ_DEFINE(g_pgfsk_tx_queue, sizeof(struct pgfsk_frame),PGFSK_LINK_QUEUE_DEPTH, 4);
K_MSGQ_DEFINE(g_pgfsk_rx_queue, sizeof(struct pgfsk_frame),PGFSK_LINK_QUEUE_DEPTH, 4);

static K_THREAD_STACK_DEFINE(g_pgfsk_link_thread_stack, PGFSK_LINK_THREAD_STACK_SIZE);
static struct k_thread g_pgfsk_link_thread;

static void pgfsk_link_thread(void *arg1, void *arg2, void *arg3);

static bool pgfsk_link_seq_gap_from_expected(uint16_t expected, uint16_t seq, uint16_t *gap)
{
	uint16_t delta = (uint16_t)(seq - expected);

	if (delta == 0U) {
		*gap = 0U;
		return true;
	}

	if (delta < 0x8000U) {
		*gap = delta;
		return true;
	}

	return false;
}

static void pgfsk_link_reset_runtime(enum pgfsk_link_state service_state)
{
	g_link.service_state = service_state;
	g_link.state = PGFSK_STATE_LISTEN;
	memset(&g_link.stats, 0, sizeof(g_link.stats));
	g_link.rx_deadline_tick = 0U;
	g_link.consecutive_rx_misses = 0U;
	g_link.in_service_since_cyc = 0U;
	g_link.next_tx_seq = 0U;
	g_link.last_rx_seq = 0U;
	g_link.have_last_rx_seq = false;
	memset(&g_link.prepared_tx_packet, 0, sizeof(g_link.prepared_tx_packet));
}

static bool pgfsk_link_abort_enable(void)
{
	pgfsk_hw_stop();

	g_link.service_state = PGFSK_LINK_STATE_NO_SERVICE;
	g_link.rx_deadline_tick = 0U;

	return false;
}

static bool pgfsk_link_start_radio(void)
{
	uint32_t now_tick;
	uint32_t deadline_tick;

	pgfsk_hw_start();
	pgfsk_hw_reset_stats();

	now_tick = pgfsk_hw_get_tick();
	deadline_tick = now_tick + PGFSK_LINK_FIXED_TICK_US + pgfsk_link_random_probe_jitter();

	g_link.rx_deadline_tick = deadline_tick;

	if (!pgfsk_hw_start_listen()) {
		return pgfsk_link_abort_enable();
	}

	/* PACKETPTR is double-buffered, but startup still needs the radio to be
	 * actively in RX before the next TX packet is pre-armed.
	 */
	if (!pgfsk_hw_wait_for_rx_active()) {
		return pgfsk_link_abort_enable();
	}

	pgfsk_link_compose_packet(&g_link.prepared_tx_packet, g_link.next_tx_seq);
	if (!pgfsk_hw_prepare_tx(&g_link.prepared_tx_packet)) {
		return pgfsk_link_abort_enable();
	}

	pgfsk_hw_set_deadline(deadline_tick);
	return true;
}

static uint32_t pgfsk_link_random_probe_jitter(void)
{
	static uint32_t probe_prng_state;
	uint32_t x;

	if (PGFSK_LINK_RANDOM_TICKS_US == 0U) {
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
	return x % (PGFSK_LINK_RANDOM_TICKS_US + 1U);
}

static void pgfsk_link_record_loss_burst(uint32_t burst_len)
{
	if (burst_len == 0U) {
		return;
	}

	if (burst_len == 1U) {
		g_link.stats.loss_burst_1_count++;
	} else if (burst_len == 2U) {
		g_link.stats.loss_burst_2_count++;
	} else if (burst_len <= 4U) {
		g_link.stats.loss_burst_3_4_count++;
	} else {
		g_link.stats.loss_burst_5_plus_count++;
	}

	if (burst_len > g_link.stats.max_loss_burst_len) {
		g_link.stats.max_loss_burst_len = burst_len;
	}
}

static void pgfsk_link_enter_in_service(void)
{
	if (g_link.service_state == PGFSK_LINK_STATE_IN_SERVICE) {
		return;
	}

	g_link.service_state = PGFSK_LINK_STATE_IN_SERVICE;
	g_link.consecutive_rx_misses = 0U;
	g_link.in_service_since_cyc = k_cycle_get_64();
	LOG_INF("service established");
}

static void pgfsk_link_enter_no_service(uint32_t now_tick)
{
	bool was_in_service = (g_link.service_state == PGFSK_LINK_STATE_IN_SERVICE);

	if (was_in_service) {
		g_link.stats.outage_count++;
	}

	g_link.service_state = PGFSK_LINK_STATE_NO_SERVICE;
	g_link.state = PGFSK_STATE_LISTEN;
	g_link.consecutive_rx_misses = 0U;
	g_link.in_service_since_cyc = 0U;
	g_link.have_last_rx_seq = false;
	g_link.rx_deadline_tick = now_tick + PGFSK_LINK_FIXED_TICK_US + pgfsk_link_random_probe_jitter();

	/* Re-enter RX and pre-arm the already composed TX packet. */
	if (!pgfsk_hw_start_listen()) {
		LOG_ERR("failed to restart RX listen in no-service");
		(void)pgfsk_link_abort_enable();
		return;
	}

	if (!pgfsk_hw_wait_for_rx_active()) {
		LOG_ERR("timed out waiting for RX active in no-service");
		(void)pgfsk_link_abort_enable();
		return;
	}

	if (!pgfsk_hw_prepare_tx(&g_link.prepared_tx_packet)) {
		LOG_ERR("failed to pre-arm TX in no-service");
		(void)pgfsk_link_abort_enable();
		return;
	}

	pgfsk_hw_set_deadline(g_link.rx_deadline_tick);

	if (was_in_service) {
		LOG_WRN("service outage (listen timeouts)");
	}
}

static void pgfsk_link_record_in_service_rx(uint16_t seq)
{
	if (g_link.have_last_rx_seq) {
		uint16_t expected = g_link.last_rx_seq + 1U;
		uint16_t gap;

		if (pgfsk_link_seq_gap_from_expected(expected, seq, &gap) && gap > 0U) {
			g_link.stats.packets_lost_in_service += gap;
			pgfsk_link_record_loss_burst(gap);
		}
	}

	g_link.last_rx_seq = seq;
	g_link.have_last_rx_seq = true;
}

static void pgfsk_link_queue_rx_frame(const struct pgfsk_hw_event *event)
{
	struct pgfsk_frame frame;
	const struct pgfsk_packet *packet;
	size_t payload_len;

	if (event == NULL) {
		__ASSERT_NO_MSG(0);
		return;
	}

	packet = &event->packet;
	if (packet->length < PGFSK_PACKET_METADATA_LEN ||
	    (packet->length - PGFSK_PACKET_METADATA_LEN) > PGFSK_PAYLOAD_MAX_LEN) {
		return;
	}

	payload_len = packet->length - PGFSK_PACKET_METADATA_LEN;
	frame = (struct pgfsk_frame){
		.seq = packet->seq,
		.len = payload_len,
		.rssi_dbm = event->rssi_dbm,
	};
	memcpy(frame.payload, packet->data, payload_len);
	(void)k_msgq_put(&g_pgfsk_rx_queue, &frame, K_NO_WAIT);
}

static void pgfsk_link_compose_packet(struct pgfsk_packet *packet, uint16_t seq)
{
	struct pgfsk_frame frame;
	bool has_payload;

	if (packet == NULL) {
		return;
	}

	has_payload = (k_msgq_get(&g_pgfsk_tx_queue, &frame, K_NO_WAIT) == 0);

	memset(packet, 0, sizeof(*packet));
	packet->length = PGFSK_PACKET_METADATA_LEN;
	packet->seq = seq;

	if (!has_payload) {
		return;
	}

	packet->length = PGFSK_PACKET_METADATA_LEN + frame.len;
	memcpy(packet->data, frame.payload, frame.len);
}

static void pgfsk_link_handle_rx_address(const struct pgfsk_hw_event *event)
{
	if (event == NULL) {
		__ASSERT_NO_MSG(0);
		return;
	}

	if (g_link.state != PGFSK_STATE_LISTEN) {
		return;
	}

	g_link.state = PGFSK_STATE_IN_RX;
	g_link.rx_deadline_tick = event->tick + PGFSK_LINK_MAX_PACKET_AIRTIME_US;
	pgfsk_hw_set_deadline(g_link.rx_deadline_tick);
}

static void pgfsk_link_handle_rx_ok(const struct pgfsk_hw_event *event)
{
	if (event == NULL) {
		__ASSERT_NO_MSG(0);
		return;
	}

	if (g_link.state != PGFSK_STATE_LISTEN &&
	    g_link.state != PGFSK_STATE_IN_RX) {
		return;
	}

	pgfsk_link_queue_rx_frame(event);
	g_link.consecutive_rx_misses = 0U;
	pgfsk_link_enter_in_service();
	pgfsk_link_record_in_service_rx(event->packet.seq);
	pgfsk_hw_clear_deadline();
	g_link.state = PGFSK_STATE_IN_TX;
}

static void pgfsk_link_handle_rx_bad(void)
{
	if (g_link.state != PGFSK_STATE_LISTEN && g_link.state != PGFSK_STATE_IN_RX) {
		return;
	}

	g_link.consecutive_rx_misses = 0U;
	pgfsk_hw_clear_deadline();
	g_link.state = PGFSK_STATE_IN_TX;
}

static void pgfsk_link_handle_tx_end(const struct pgfsk_hw_event *event)
{
	if (event == NULL) {
		__ASSERT_NO_MSG(0);
		return;
	}

	if (g_link.state != PGFSK_STATE_IN_TX) {
		return;
	}

	g_link.next_tx_seq++;
	g_link.state = PGFSK_STATE_LISTEN;

	if (g_link.service_state == PGFSK_LINK_STATE_IN_SERVICE) {
		g_link.rx_deadline_tick = event->tick + PGFSK_LINK_MAX_PACKET_AIRTIME_US;
	} else {
		g_link.rx_deadline_tick = event->tick + PGFSK_LINK_MAX_PACKET_AIRTIME_US + pgfsk_link_random_probe_jitter();
	}

	pgfsk_link_compose_packet(&g_link.prepared_tx_packet, g_link.next_tx_seq);
	if (!pgfsk_hw_prepare_tx(&g_link.prepared_tx_packet)) {
		LOG_WRN("failed to arm next reply");
	}

	pgfsk_hw_set_deadline(g_link.rx_deadline_tick);
}

static void pgfsk_link_handle_in_rx_timeout(void)
{
	g_link.stats.rx_incomplete_count++;
	g_link.consecutive_rx_misses = 0U;

	g_link.state = PGFSK_STATE_IN_TX;
	if (!pgfsk_hw_trigger_prepared_tx()) {
		g_link.stats.tx_trigger_fail_count++;
		pgfsk_link_enter_no_service(pgfsk_hw_get_tick());
	}
}

static void pgfsk_link_handle_listen_timeout(uint32_t now_tick)
{
	if (g_link.service_state == PGFSK_LINK_STATE_IN_SERVICE) {
		g_link.consecutive_rx_misses++;
		if (g_link.consecutive_rx_misses >= PGFSK_LINK_SYNC_LOSS_TURNS) {
			pgfsk_link_enter_no_service(now_tick);
			return;
		}
	}

	g_link.state = PGFSK_STATE_IN_TX;
	if (!pgfsk_hw_trigger_prepared_tx()) {
		g_link.stats.tx_trigger_fail_count++;
		pgfsk_link_enter_no_service(now_tick);
	}
}

static void pgfsk_link_handle_timeout(const struct pgfsk_hw_event *event)
{
	if (event == NULL) {
		__ASSERT_NO_MSG(0);
		return;
	}

	if (g_link.state == PGFSK_STATE_LISTEN) {
		pgfsk_link_handle_listen_timeout(event->tick);
		return;
	}

	if (g_link.state == PGFSK_STATE_IN_RX) {
		pgfsk_link_handle_in_rx_timeout();
	}
}

static void pgfsk_link_handle_radio_event(const struct pgfsk_hw_event *event)
{
	if (event == NULL) {
		__ASSERT_NO_MSG(0);
		return;
	}

	switch (event->type) {
	case PGFSK_HW_EVENT_RX_ADDRESS:
		pgfsk_link_handle_rx_address(event);
		break;
	case PGFSK_HW_EVENT_RX_OK:
		pgfsk_link_handle_rx_ok(event);
		break;
	case PGFSK_HW_EVENT_RX_BAD:
		pgfsk_link_handle_rx_bad();
		break;
	case PGFSK_HW_EVENT_TX_END:
		pgfsk_link_handle_tx_end(event);
		break;
	case PGFSK_HW_EVENT_TIMEOUT:
		pgfsk_link_handle_timeout(event);
		break;
	default:
		break;
	}
}

static bool pgfsk_link_start_with_role(void (*set_role)(void))
{
	bool started;

	pgfsk_hw_init();

	k_mutex_lock(&g_link.mutex_lock, K_FOREVER);
	pgfsk_link_reset_runtime(PGFSK_LINK_STATE_NO_SERVICE);
	set_role();
	started = pgfsk_link_start_radio();
	k_mutex_unlock(&g_link.mutex_lock);

	k_thread_create(&g_pgfsk_link_thread, g_pgfsk_link_thread_stack,
			K_THREAD_STACK_SIZEOF(g_pgfsk_link_thread_stack),
			pgfsk_link_thread, NULL, NULL, NULL,
			PGFSK_LINK_THREAD_PRIORITY, 0, K_NO_WAIT);

	return started;
}

bool pgfsk_link_start_dongle(void)
{
	return pgfsk_link_start_with_role(pgfsk_hw_set_role_dongle);
}

bool pgfsk_link_start_headset(void)
{
	return pgfsk_link_start_with_role(pgfsk_hw_set_role_headset);
}

enum pgfsk_link_state pgfsk_link_get_state(void)
{
	enum pgfsk_link_state state;

	k_mutex_lock(&g_link.mutex_lock, K_FOREVER);
	state = g_link.service_state;
	k_mutex_unlock(&g_link.mutex_lock);
	return state;
}

void pgfsk_link_get_report(struct pgfsk_link_report *report)
{
	struct pgfsk_hw_stats hw_stats;
	struct link_stats stats;
	enum pgfsk_link_state state;
	uint64_t in_service_since_cyc;
	uint64_t time_in_service_us = 0U;

	if (report == NULL) {
		return;
	}

	pgfsk_hw_get_stats(&hw_stats);

	k_mutex_lock(&g_link.mutex_lock, K_FOREVER);
	stats = g_link.stats;
	state = g_link.service_state;
	in_service_since_cyc = g_link.in_service_since_cyc;
	k_mutex_unlock(&g_link.mutex_lock);

	if (state == PGFSK_LINK_STATE_IN_SERVICE && in_service_since_cyc != 0U) {
		time_in_service_us = k_cyc_to_us_floor64(k_cycle_get_64() - in_service_since_cyc);
	}

	*report = (struct pgfsk_link_report){
		.packets_tx = hw_stats.packets_tx,
		.rx_ok_count = hw_stats.rx_ok_count,
		.packets_lost_in_service = stats.packets_lost_in_service,
		.loss_burst_1_count = stats.loss_burst_1_count,
		.loss_burst_2_count = stats.loss_burst_2_count,
		.loss_burst_3_4_count = stats.loss_burst_3_4_count,
		.loss_burst_5_plus_count = stats.loss_burst_5_plus_count,
		.max_loss_burst_len = stats.max_loss_burst_len,
		.crc_error_count = hw_stats.crc_errors,
		.deadline_late_count = hw_stats.deadline_late_count,
		.rx_incomplete_count = stats.rx_incomplete_count,
		.tx_trigger_fail_count = stats.tx_trigger_fail_count,
		.outage_count = stats.outage_count,
		.time_in_service_us = time_in_service_us,
		.last_rssi_dbm = hw_stats.last_rssi_dbm,
		.state = state,
	};
}

bool pgfsk_link_tx_enqueue(const struct pgfsk_frame *frame, k_timeout_t timeout)
{
	if (frame == NULL || frame->len > PGFSK_PAYLOAD_MAX_LEN) {
		return false;
	}

	return k_msgq_put(&g_pgfsk_tx_queue, frame, timeout) == 0;
}

bool pgfsk_link_rx_dequeue(struct pgfsk_frame *frame, k_timeout_t timeout)
{
	if (frame == NULL) {
		return false;
	}

	return k_msgq_get(&g_pgfsk_rx_queue, frame, timeout) == 0;
}

static void pgfsk_link_thread(void *arg1, void *arg2, void *arg3)
{
	struct pgfsk_hw_event event;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (1) {
		if (!pgfsk_hw_dequeue_event(&event, K_FOREVER)) {
			continue;
		}

		k_mutex_lock(&g_link.mutex_lock, K_FOREVER);
		pgfsk_link_handle_radio_event(&event);
		k_mutex_unlock(&g_link.mutex_lock);
	}
}
