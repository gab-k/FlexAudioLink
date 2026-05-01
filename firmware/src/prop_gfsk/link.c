#include "prop_gfsk/link.h"

#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(pgfsk_link, CONFIG_LOG_DEFAULT_LEVEL);

#define PGFSK_LINK_THREAD_STACK_SIZE       2048
#define PGFSK_LINK_THREAD_PRIORITY         6
#define PGFSK_LINK_QUEUE_DEPTH             4
#define PGFSK_LINK_SYNC_LOSS_TURNS         8U

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
	struct link_stats stats;
	uint8_t consecutive_rx_misses;
	uint64_t in_service_since_cyc;
	uint16_t next_tx_seq;
	uint16_t last_rx_seq;
	bool have_last_rx_seq;
	struct k_mutex mutex_lock;
};

static struct pgfsk_link_runtime g_link = {
	.service_state = PGFSK_LINK_STATE_NO_SERVICE,
	.mutex_lock = Z_MUTEX_INITIALIZER(g_link.mutex_lock),
};

static bool pgfsk_link_copy_to_hw_rb(void);

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

static uint16_t pgfsk_link_next_payload_seq(uint16_t seq)
{
	seq++;
	if (seq == PGFSK_KEEPALIVE_SEQ) {
		seq++;
	}

	return seq;
}

static void pgfsk_link_reset_runtime(enum pgfsk_link_state service_state)
{
	g_link.service_state = service_state;
	memset(&g_link.stats, 0, sizeof(g_link.stats));
	g_link.consecutive_rx_misses = 0U;
	g_link.in_service_since_cyc = 0U;
	g_link.next_tx_seq = 0U;
	g_link.last_rx_seq = 0U;
	g_link.have_last_rx_seq = false;
}

static bool pgfsk_link_abort_enable(void)
{
	pgfsk_hw_stop();

	g_link.service_state = PGFSK_LINK_STATE_NO_SERVICE;

	return false;
}

static bool pgfsk_link_start_radio(void)
{
	pgfsk_hw_start();
	while (pgfsk_link_copy_to_hw_rb()) {
	}

	return true;
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

static void pgfsk_link_mark_no_service(void)
{
	bool was_in_service = (g_link.service_state == PGFSK_LINK_STATE_IN_SERVICE);

	if (was_in_service) {
		g_link.stats.outage_count++;
	}

	g_link.service_state = PGFSK_LINK_STATE_NO_SERVICE;
	g_link.consecutive_rx_misses = 0U;
	g_link.in_service_since_cyc = 0U;
	g_link.have_last_rx_seq = false;

	if (was_in_service) {
		LOG_WRN("service outage (listen timeouts)");
	}
}

static void pgfsk_link_record_in_service_rx(uint16_t seq)
{
	if (g_link.have_last_rx_seq) {
		uint16_t expected = pgfsk_link_next_payload_seq(g_link.last_rx_seq);
		uint16_t gap;

		if (pgfsk_link_seq_gap_from_expected(expected, seq, &gap) && gap > 0U) {
			g_link.stats.packets_lost_in_service += gap;
			pgfsk_link_record_loss_burst(gap);
		}
	}

	g_link.last_rx_seq = seq;
	g_link.have_last_rx_seq = true;
}

static void pgfsk_link_queue_rx_frame(const struct pgfsk_packet *packet, int16_t rssi_dbm)
{
	struct pgfsk_frame frame;
	size_t payload_len;

	if (packet == NULL) {
		__ASSERT_NO_MSG(0);
		return;
	}

	if (packet->length < PGFSK_PACKET_METADATA_LEN ||
	    (packet->length - PGFSK_PACKET_METADATA_LEN) > PGFSK_PAYLOAD_MAX_LEN) {
		return;
	}

	payload_len = packet->length - PGFSK_PACKET_METADATA_LEN;
	frame = (struct pgfsk_frame){
		.seq = packet->seq,
		.len = payload_len,
		.rssi_dbm = rssi_dbm,
	};
	memcpy(frame.payload, packet->data, payload_len);
	(void)k_msgq_put(&g_pgfsk_rx_queue, &frame, K_NO_WAIT);
}

static bool pgfsk_link_is_keepalive_packet(const struct pgfsk_packet *packet)
{
	if (packet == NULL) {
		__ASSERT_NO_MSG(0);
		return false;
	}

	return packet->length == PGFSK_KEEPALIVE_LEN && packet->seq == PGFSK_KEEPALIVE_SEQ;
}

static bool pgfsk_link_copy_to_hw_rb(void)
{
	struct pgfsk_packet *packet;
	struct pgfsk_frame frame;

	packet = pgfsk_hw_tx_get_wr_ptr();
	if (packet == NULL) {
		return false;
	}

	if (k_msgq_get(&g_pgfsk_tx_queue, &frame, K_NO_WAIT) != 0) {
		return false;
	}

	memset(packet, 0, sizeof(*packet));
	packet->length = PGFSK_PACKET_METADATA_LEN + frame.len;
	packet->seq = g_link.next_tx_seq;
	memcpy(packet->data, frame.payload, frame.len);

	pgfsk_hw_tx_advance_wr_idx();
	g_link.next_tx_seq = pgfsk_link_next_payload_seq(g_link.next_tx_seq);
	return true;
}

static void pgfsk_link_handle_rx_ok(const struct pgfsk_hw_event *event)
{
	const struct pgfsk_packet *packet;
	bool is_keepalive;

	if (event == NULL) {
		__ASSERT_NO_MSG(0);
		return;
	}

	packet = pgfsk_hw_rx_get_rd_ptr();
	if (packet == NULL) {
		__ASSERT_NO_MSG(0);
		return;
	}

	is_keepalive = pgfsk_link_is_keepalive_packet(packet);
	if (packet->length < PGFSK_PACKET_METADATA_LEN ||
	    (packet->length - PGFSK_PACKET_METADATA_LEN) > PGFSK_PAYLOAD_MAX_LEN ||
	    (!is_keepalive && packet->length == PGFSK_PACKET_METADATA_LEN)) {
		pgfsk_hw_rx_advance_rd_idx();
		return;
	}

	g_link.consecutive_rx_misses = 0U;
	pgfsk_link_enter_in_service();

	if (!is_keepalive) {
		pgfsk_link_queue_rx_frame(packet, event->rssi_dbm);
		pgfsk_link_record_in_service_rx(packet->seq);
	}

	pgfsk_hw_rx_advance_rd_idx();
}

static void pgfsk_link_handle_rx_bad(const struct pgfsk_hw_event *event)
{
	if (event == NULL) {
		__ASSERT_NO_MSG(0);
		return;
	}

	g_link.consecutive_rx_misses = 0U;
	pgfsk_hw_rx_advance_rd_idx();
}

static void pgfsk_link_handle_tx_end(const struct pgfsk_hw_event *event)
{
	if (event == NULL) {
		__ASSERT_NO_MSG(0);
		return;
	}

	while (pgfsk_link_copy_to_hw_rb()) {
	}
}

static void pgfsk_link_handle_listen_timeout(void)
{
	if (g_link.service_state == PGFSK_LINK_STATE_IN_SERVICE) {
		g_link.consecutive_rx_misses++;
		if (g_link.consecutive_rx_misses >= PGFSK_LINK_SYNC_LOSS_TURNS) {
			pgfsk_link_mark_no_service();
			return;
		}
	} else {
		LOG_WRN_RATELIMIT_RATE(10000, "Waiting for PGFSK peer...");
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
	if (frame == NULL || frame->len == 0U || frame->len > PGFSK_PAYLOAD_MAX_LEN) {
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
		switch (event.type) {
		case PGFSK_HW_EVENT_RX_OK:
			pgfsk_link_handle_rx_ok(&event);
			break;
		case PGFSK_HW_EVENT_RX_BAD:
			pgfsk_link_handle_rx_bad(&event);
			break;
		case PGFSK_HW_EVENT_TX_END:
			pgfsk_link_handle_tx_end(&event);
			break;
		case PGFSK_HW_EVENT_RX_INCOMPLETE:
			g_link.stats.rx_incomplete_count++;
			g_link.consecutive_rx_misses = 0U;
			break;
		case PGFSK_HW_EVENT_LISTEN_TIMEOUT:
			pgfsk_link_handle_listen_timeout();
			break;
		case PGFSK_HW_EVENT_TX_TRIGGER_FAILED:
			g_link.stats.tx_trigger_fail_count++;
			pgfsk_link_mark_no_service();
			LOG_ERR("failed to trigger prepared TX");
			(void)pgfsk_link_abort_enable();
			break;
		default:
			break;
		}
		k_mutex_unlock(&g_link.mutex_lock);
	}
}
