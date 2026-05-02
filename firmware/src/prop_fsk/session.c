#include "prop_fsk/session.h"

#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(pfsk_session, CONFIG_LOG_DEFAULT_LEVEL);

#define PFSK_SESSION_THREAD_STACK_SIZE       2048
#define PFSK_SESSION_THREAD_PRIORITY         6
#define PFSK_SESSION_QUEUE_DEPTH             4
#define PFSK_SESSION_SYNC_LOSS_TURNS         8U

struct session_stats {
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

struct pfsk_session_runtime {
	enum pfsk_session_state service_state;
	struct session_stats stats;
	uint8_t consecutive_rx_misses;
	uint64_t in_service_since_cyc;
	uint16_t next_tx_seq;
	uint16_t last_rx_seq;
	bool have_last_rx_seq;
	struct k_mutex mutex_lock;
};

static struct pfsk_session_runtime session = {
	.service_state = PFSK_SESSION_STATE_NO_SERVICE,
	.mutex_lock = Z_MUTEX_INITIALIZER(session.mutex_lock),
};

static bool pfsk_session_copy_to_radio_rb(void);

K_MSGQ_DEFINE(pfsk_tx_queue, sizeof(struct pfsk_packet), PFSK_SESSION_QUEUE_DEPTH, 4);
K_MSGQ_DEFINE(pfsk_rx_queue, sizeof(struct pfsk_packet), PFSK_SESSION_QUEUE_DEPTH, 4);

static K_THREAD_STACK_DEFINE(pfsk_session_thread_stack, PFSK_SESSION_THREAD_STACK_SIZE);
static struct k_thread pfsk_session_thread_data;

static void pfsk_session_thread(void *arg1, void *arg2, void *arg3);

static bool pfsk_session_seq_gap_from_expected(uint16_t expected, uint16_t seq, uint16_t *gap)
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

static uint16_t pfsk_session_next_payload_seq(uint16_t seq)
{
	seq++;
	if (seq == PFSK_KEEPALIVE_SEQ) {
		seq++;
	}

	return seq;
}

static void pfsk_session_reset_runtime(enum pfsk_session_state service_state)
{
	session.service_state = service_state;
	memset(&session.stats, 0, sizeof(session.stats));
	session.consecutive_rx_misses = 0U;
	session.in_service_since_cyc = 0U;
	session.next_tx_seq = 0U;
	session.last_rx_seq = 0U;
	session.have_last_rx_seq = false;
}

static bool pfsk_session_abort_enable(void)
{
	pfsk_radio_stop();

	session.service_state = PFSK_SESSION_STATE_NO_SERVICE;

	return false;
}

static bool pfsk_session_start_radio(void)
{
	pfsk_radio_start();
	while (pfsk_session_copy_to_radio_rb()) {
	}

	return true;
}

static void pfsk_session_record_loss_burst(uint32_t burst_len)
{
	if (burst_len == 0U) {
		return;
	}

	if (burst_len == 1U) {
		session.stats.loss_burst_1_count++;
	} else if (burst_len == 2U) {
		session.stats.loss_burst_2_count++;
	} else if (burst_len <= 4U) {
		session.stats.loss_burst_3_4_count++;
	} else {
		session.stats.loss_burst_5_plus_count++;
	}

	if (burst_len > session.stats.max_loss_burst_len) {
		session.stats.max_loss_burst_len = burst_len;
	}
}

static void pfsk_session_enter_in_service(void)
{
	if (session.service_state == PFSK_SESSION_STATE_IN_SERVICE) {
		return;
	}

	session.service_state = PFSK_SESSION_STATE_IN_SERVICE;
	session.consecutive_rx_misses = 0U;
	session.in_service_since_cyc = k_cycle_get_64();
	LOG_INF("service established");
}

static void pfsk_session_mark_no_service(void)
{
	bool was_in_service = (session.service_state == PFSK_SESSION_STATE_IN_SERVICE);

	if (was_in_service) {
		session.stats.outage_count++;
	}

	session.service_state = PFSK_SESSION_STATE_NO_SERVICE;
	session.consecutive_rx_misses = 0U;
	session.in_service_since_cyc = 0U;
	session.have_last_rx_seq = false;

	if (was_in_service) {
		LOG_WRN("service outage (listen timeouts)");
	}
}

static void pfsk_session_record_in_service_rx(uint16_t seq)
{
	if (session.have_last_rx_seq) {
		uint16_t expected = pfsk_session_next_payload_seq(session.last_rx_seq);
		uint16_t gap;

		if (pfsk_session_seq_gap_from_expected(expected, seq, &gap) && gap > 0U) {
			session.stats.packets_lost_in_service += gap;
			pfsk_session_record_loss_burst(gap);
		}
	}

	session.last_rx_seq = seq;
	session.have_last_rx_seq = true;
}

static bool pfsk_session_packet_len_is_valid(uint8_t packet_len)
{
	return packet_len > PFSK_PACKET_METADATA_LEN &&
	       packet_len <= PFSK_PACKET_MAX_LEN;
}

static bool pfsk_session_is_keepalive_packet(const struct pfsk_packet *packet)
{
	if (packet == NULL) {
		__ASSERT_NO_MSG(0);
		return false;
	}

	return packet->length == PFSK_KEEPALIVE_LEN && packet->seq == PFSK_KEEPALIVE_SEQ;
}

static bool pfsk_session_copy_to_radio_rb(void)
{
	struct pfsk_packet *radio_packet;
	struct pfsk_packet queued_packet;
	uint8_t payload_bytes;

	radio_packet = pfsk_radio_tx_get_wr_ptr();
	if (radio_packet == NULL) {
		return false;
	}

	if (k_msgq_get(&pfsk_tx_queue, &queued_packet, K_NO_WAIT) != 0) {
		return false;
	}

	if (!pfsk_session_packet_len_is_valid(queued_packet.length)) {
		LOG_ERR("copy_to_radio_rb packet length is invalid!");
		return false;
	}

	payload_bytes = queued_packet.length - PFSK_PACKET_METADATA_LEN;
	memset(radio_packet, 0, sizeof(*radio_packet));
	radio_packet->length = queued_packet.length;
	radio_packet->seq = session.next_tx_seq;
	memcpy(radio_packet->payload, queued_packet.payload, payload_bytes);

	pfsk_radio_tx_advance_wr_idx();
	session.next_tx_seq = pfsk_session_next_payload_seq(session.next_tx_seq);
	return true;
}

static void pfsk_session_handle_rx_ok(const struct pfsk_radio_event *event)
{
	const struct pfsk_packet *packet;
	bool is_keepalive;

	if (event == NULL) {
		__ASSERT_NO_MSG(0);
		return;
	}

	packet = pfsk_radio_rx_get_rd_ptr();
	if (packet == NULL) {
		__ASSERT_NO_MSG(0);
		return;
	}

	is_keepalive = pfsk_session_is_keepalive_packet(packet);
	if (!pfsk_session_packet_len_is_valid(packet->length)) {
		LOG_ERR("rx_handle_ok packet length is invalid!");
		pfsk_radio_rx_advance_rd_idx();
		return;
	}

	session.consecutive_rx_misses = 0U;
	pfsk_session_enter_in_service();

	if (!is_keepalive) {
		(void)k_msgq_put(&pfsk_rx_queue, packet, K_NO_WAIT);
		pfsk_session_record_in_service_rx(packet->seq);
	}

	pfsk_radio_rx_advance_rd_idx();
}

static void pfsk_session_handle_rx_bad(const struct pfsk_radio_event *event)
{
	if (event == NULL) {
		__ASSERT_NO_MSG(0);
		return;
	}

	session.consecutive_rx_misses = 0U;
	pfsk_radio_rx_advance_rd_idx();
}

static void pfsk_session_handle_tx_end(const struct pfsk_radio_event *event)
{
	if (event == NULL) {
		__ASSERT_NO_MSG(0);
		return;
	}

	while (pfsk_session_copy_to_radio_rb()) {
	}
}

static void pfsk_session_handle_listen_timeout(void)
{
	if (session.service_state == PFSK_SESSION_STATE_IN_SERVICE) {
		session.consecutive_rx_misses++;
		if (session.consecutive_rx_misses >= PFSK_SESSION_SYNC_LOSS_TURNS) {
			pfsk_session_mark_no_service();
			return;
		}
	} else {
		LOG_WRN_RATELIMIT_RATE(10000, "Waiting for PFSK peer...");
	}
}

static bool pfsk_session_start_with_role(void (*set_role)(void))
{
	bool started;

	pfsk_radio_init();

	k_mutex_lock(&session.mutex_lock, K_FOREVER);
	pfsk_session_reset_runtime(PFSK_SESSION_STATE_NO_SERVICE);
	set_role();
	started = pfsk_session_start_radio();
	k_mutex_unlock(&session.mutex_lock);

	k_thread_create(&pfsk_session_thread_data, pfsk_session_thread_stack,
			K_THREAD_STACK_SIZEOF(pfsk_session_thread_stack),
			pfsk_session_thread, NULL, NULL, NULL,
			PFSK_SESSION_THREAD_PRIORITY, 0, K_NO_WAIT);

	return started;
}

bool pfsk_session_start_dongle(void)
{
	return pfsk_session_start_with_role(pfsk_radio_set_role_dongle);
}

bool pfsk_session_start_headset(void)
{
	return pfsk_session_start_with_role(pfsk_radio_set_role_headset);
}

enum pfsk_session_state pfsk_session_get_state(void)
{
	enum pfsk_session_state state;

	k_mutex_lock(&session.mutex_lock, K_FOREVER);
	state = session.service_state;
	k_mutex_unlock(&session.mutex_lock);
	return state;
}

void pfsk_session_get_report(struct pfsk_session_report *report)
{
	struct pfsk_radio_stats radio_stats;
	struct session_stats stats;
	enum pfsk_session_state state;
	uint64_t in_service_since_cyc;
	uint64_t time_in_service_us = 0U;

	if (report == NULL) {
		return;
	}

	pfsk_radio_get_stats(&radio_stats);

	k_mutex_lock(&session.mutex_lock, K_FOREVER);
	stats = session.stats;
	state = session.service_state;
	in_service_since_cyc = session.in_service_since_cyc;
	k_mutex_unlock(&session.mutex_lock);

	if (state == PFSK_SESSION_STATE_IN_SERVICE && in_service_since_cyc != 0U) {
		time_in_service_us = k_cyc_to_us_floor64(k_cycle_get_64() - in_service_since_cyc);
	}

	*report = (struct pfsk_session_report){
		.packets_tx = radio_stats.packets_tx,
		.rx_ok_count = radio_stats.rx_ok_count,
		.packets_lost_in_service = stats.packets_lost_in_service,
		.loss_burst_1_count = stats.loss_burst_1_count,
		.loss_burst_2_count = stats.loss_burst_2_count,
		.loss_burst_3_4_count = stats.loss_burst_3_4_count,
		.loss_burst_5_plus_count = stats.loss_burst_5_plus_count,
		.max_loss_burst_len = stats.max_loss_burst_len,
		.crc_error_count = radio_stats.crc_errors,
		.deadline_late_count = radio_stats.deadline_late_count,
		.rx_incomplete_count = stats.rx_incomplete_count,
		.tx_trigger_fail_count = stats.tx_trigger_fail_count,
		.outage_count = stats.outage_count,
		.time_in_service_us = time_in_service_us,
		.last_rssi_dbm = radio_stats.last_rssi_dbm,
		.state = state,
	};
}

bool pfsk_session_tx_enqueue(const struct pfsk_packet *packet, k_timeout_t timeout)
{
	if (packet == NULL) {
		LOG_ERR("tx_enqueue packet pointer is NULL!");
		return false;
	}

	if (!pfsk_session_packet_len_is_valid(packet->length)) {
		LOG_ERR("tx_enqueue packet length is invalid!");
		return false;
	}

	return k_msgq_put(&pfsk_tx_queue, packet, timeout) == 0;
}

bool pfsk_session_rx_dequeue(struct pfsk_packet *packet, k_timeout_t timeout)
{
	if (packet == NULL) {
		LOG_ERR("rx_dequeue packet pointer is NULL!");
		return false;
	}

	return k_msgq_get(&pfsk_rx_queue, packet, timeout) == 0;
}

static void pfsk_session_thread(void *arg1, void *arg2, void *arg3)
{
	struct pfsk_radio_event event;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (1) {
		if (!pfsk_radio_dequeue_event(&event, K_FOREVER)) {
			continue;
		}

		k_mutex_lock(&session.mutex_lock, K_FOREVER);
		switch (event.type) {
		case PFSK_RADIO_EVENT_RX_OK:
			pfsk_session_handle_rx_ok(&event);
			break;
		case PFSK_RADIO_EVENT_RX_BAD:
			pfsk_session_handle_rx_bad(&event);
			break;
		case PFSK_RADIO_EVENT_TX_END:
			pfsk_session_handle_tx_end(&event);
			break;
		case PFSK_RADIO_EVENT_RX_INCOMPLETE:
			session.stats.rx_incomplete_count++;
			session.consecutive_rx_misses = 0U;
			break;
		case PFSK_RADIO_EVENT_LISTEN_TIMEOUT:
			pfsk_session_handle_listen_timeout();
			break;
		case PFSK_RADIO_EVENT_TX_TRIGGER_FAILED:
			session.stats.tx_trigger_fail_count++;
			pfsk_session_mark_no_service();
			LOG_ERR("failed to trigger prepared TX");
			(void)pfsk_session_abort_enable();
			break;
		default:
			break;
		}
		k_mutex_unlock(&session.mutex_lock);
	}
}
