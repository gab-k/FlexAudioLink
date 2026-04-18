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
#define PGFSK_LINK_CONFIG_SET_TIMEOUT_MS   1000

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
	struct pgfsk_link_config config;
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
	struct k_spinlock lock;
};

struct pgfsk_link_config_request {
	struct pgfsk_link_config config;
	uint32_t seq;
};

static struct pgfsk_link_runtime g_link = {
	.config = {
		.enabled = false,
		.local_device_role = DEVICE_ROLE_HEADSET,
	},
	.service_state = PGFSK_LINK_STATE_DISABLED,
	.state = PGFSK_STATE_LISTEN,
};

static uint32_t g_pgfsk_config_request_seq;
static uint32_t g_pgfsk_config_completed_seq;
static bool g_pgfsk_config_completed_result;
static uint32_t g_pgfsk_probe_prng_state;

static uint32_t pgfsk_link_random_probe_jitter(void);
static void pgfsk_link_compose_packet(struct pgfsk_packet *packet, uint16_t seq);

K_MSGQ_DEFINE(g_pgfsk_tx_queue, sizeof(struct pgfsk_frame),PGFSK_LINK_QUEUE_DEPTH, 4);
K_MSGQ_DEFINE(g_pgfsk_rx_queue, sizeof(struct pgfsk_frame),PGFSK_LINK_QUEUE_DEPTH, 4);
K_MSGQ_DEFINE(g_pgfsk_config_queue,sizeof(struct pgfsk_link_config_request), 1, 4);
K_MUTEX_DEFINE(g_pgfsk_config_lock);
K_SEM_DEFINE(g_pgfsk_config_done_sem, 0, 1);

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

static struct pgfsk_link_config pgfsk_link_get_config(void)
{
	k_spinlock_key_t key = k_spin_lock(&g_link.lock);
	struct pgfsk_link_config config = g_link.config;

	k_spin_unlock(&g_link.lock, key);
	return config;
}

static void pgfsk_link_reset_runtime(const struct pgfsk_link_config *config)
{
	k_spinlock_key_t key;

	__ASSERT_NO_MSG(config != NULL);

	key = k_spin_lock(&g_link.lock);

	g_link.config = *config;
	g_link.service_state = config->enabled ? PGFSK_LINK_STATE_NO_SERVICE : PGFSK_LINK_STATE_DISABLED;
	g_link.state = PGFSK_STATE_LISTEN;
	memset(&g_link.stats, 0, sizeof(g_link.stats));
	g_link.rx_deadline_tick = 0U;
	g_link.consecutive_rx_misses = 0U;
	g_link.in_service_since_cyc = 0U;
	g_link.next_tx_seq = 0U;
	g_link.last_rx_seq = 0U;
	g_link.have_last_rx_seq = false;
	memset(&g_link.prepared_tx_packet, 0, sizeof(g_link.prepared_tx_packet));

	k_spin_unlock(&g_link.lock, key);
}

static bool pgfsk_link_abort_enable(void)
{
	k_spinlock_key_t key;

	pgfsk_hw_stop();

	key = k_spin_lock(&g_link.lock);
	g_link.config.enabled = false;
	g_link.service_state = PGFSK_LINK_STATE_DISABLED;
	g_link.rx_deadline_tick = 0U;
	k_spin_unlock(&g_link.lock, key);

	return false;
}

static bool pgfsk_link_start_enabled(const struct pgfsk_link_config *config)
{
	k_spinlock_key_t key;
	uint32_t now_tick;
	uint32_t deadline_tick;

	__ASSERT_NO_MSG(config != NULL);

	pgfsk_hw_set_role(config->local_device_role);
	pgfsk_hw_start();
	pgfsk_hw_reset_stats();

	now_tick = pgfsk_hw_get_tick();
	deadline_tick = now_tick + PGFSK_LINK_FIXED_TICK_US + pgfsk_link_random_probe_jitter();

	key = k_spin_lock(&g_link.lock);
	g_link.rx_deadline_tick = deadline_tick;
	k_spin_unlock(&g_link.lock, key);

	if (!pgfsk_hw_start_listen()) {
		return pgfsk_link_abort_enable();
	}

	/* PACKETPTR is double-buffered, but startup still needs the radio to actively enter RX
	 * before TX is pre-armed.
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

static void pgfsk_link_seed_probe_prng(void)
{
	uint8_t device_id[16];
	uint32_t seed = 0x6d2b79f5U ^ k_cycle_get_32();
	ssize_t len;

	len = hwinfo_get_device_id(device_id, sizeof(device_id));
	if (len > 0) {
		for (size_t i = 0; i < (size_t)len; ++i) {
			seed ^= device_id[i];
			seed *= 16777619U;
		}
	}

	if (seed == 0U) {
		seed = 0x1b873593U;
	}

	g_pgfsk_probe_prng_state = seed;
}

static uint32_t pgfsk_link_random_probe_jitter(void)
{
	uint32_t x;

	if (PGFSK_LINK_RANDOM_TICKS_US == 0U) {
		return 0U;
	}

	if (g_pgfsk_probe_prng_state == 0U) {
		pgfsk_link_seed_probe_prng();
	}

	x = g_pgfsk_probe_prng_state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	if (x == 0U) {
		x = 0x9e3779b9U;
	}

	g_pgfsk_probe_prng_state = x;
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
	bool listen_ok;
	bool tx_ok;

	if (was_in_service) {
		g_link.stats.outage_count++;
	}

	g_link.service_state = PGFSK_LINK_STATE_NO_SERVICE;
	g_link.state = PGFSK_STATE_LISTEN;
	g_link.consecutive_rx_misses = 0U;
	g_link.in_service_since_cyc = 0U;
	g_link.have_last_rx_seq = false;
	g_link.rx_deadline_tick = now_tick + PGFSK_LINK_FIXED_TICK_US + pgfsk_link_random_probe_jitter();
	listen_ok = pgfsk_hw_start_listen();
	tx_ok = listen_ok && pgfsk_hw_prepare_tx(&g_link.prepared_tx_packet);

	if (!listen_ok || !tx_ok) {
		pgfsk_hw_stop();
		pgfsk_hw_set_role(g_link.config.local_device_role);
		pgfsk_hw_start();
		listen_ok = pgfsk_hw_start_listen();
		tx_ok = listen_ok && pgfsk_hw_prepare_tx(&g_link.prepared_tx_packet);
	}

	pgfsk_hw_set_deadline(g_link.rx_deadline_tick);

	if (!listen_ok || !tx_ok) {
		LOG_ERR("failed to enter no-service listen posture");
		(void)pgfsk_link_abort_enable();
		return;
	}

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

static void pgfsk_link_trigger_timeout_tx(void)
{
	g_link.state = PGFSK_STATE_IN_TX;

	if (!pgfsk_hw_trigger_prepared_tx()) {
		g_link.stats.tx_trigger_fail_count++;
		pgfsk_link_enter_no_service(pgfsk_hw_get_tick());
	}
}

static void pgfsk_link_handle_in_rx_timeout(void)
{
	g_link.stats.rx_incomplete_count++;
	g_link.consecutive_rx_misses = 0U;
	pgfsk_link_trigger_timeout_tx();
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

	pgfsk_link_trigger_timeout_tx();
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

static void pgfsk_link_print_banner(const struct pgfsk_link_config *config)
{
	const char *role;

	if (config == NULL || !config->enabled) {
		return;
	}

	role = (config->local_device_role == DEVICE_ROLE_DONGLE) ? "dongle" : "headset";
	LOG_INF("=== FlexLink GFSK Link ===");
	LOG_INF("Role: %s", role);
	LOG_INF("Frequency: %u MHz", PGFSK_HW_FREQUENCY_MHZ);
	LOG_INF("Rate: 4 Mbps");
	LOG_INF("TX Power: +8 dBm");
	LOG_INF("Max packet airtime: %u us", PGFSK_LINK_MAX_PACKET_AIRTIME_US);
	LOG_INF("Sync loss turns: %u", PGFSK_LINK_SYNC_LOSS_TURNS);
	LOG_INF("Probe delay: %u..%u us", 
			PGFSK_LINK_FIXED_TICK_US, PGFSK_LINK_FIXED_TICK_US + 
			PGFSK_LINK_RANDOM_TICKS_US);
	LOG_INF("Dongle addr: 0x%08lX", PGFSK_HW_ADDR_DONGLE);
	LOG_INF("Headset addr: 0x%08lX", PGFSK_HW_ADDR_HEADSET);
}

static bool pgfsk_link_apply_config_internal(const struct pgfsk_link_config *config)
{
	struct pgfsk_link_config prev_config;
	bool changed;

	if (config == NULL) {
		return false;
	}

	if (config->local_device_role != DEVICE_ROLE_DONGLE &&
	    config->local_device_role != DEVICE_ROLE_HEADSET) {
		return false;
	}

	prev_config = pgfsk_link_get_config();

	pgfsk_hw_stop();
	k_msgq_purge(&g_pgfsk_tx_queue);
	k_msgq_purge(&g_pgfsk_rx_queue);

	pgfsk_link_reset_runtime(config);

	if (!config->enabled) {
		return true;
	}

	if (!pgfsk_link_start_enabled(config)) {
		return false;
	}

	changed = (!prev_config.enabled || prev_config.local_device_role != config->local_device_role);
	if (changed) {
		pgfsk_link_print_banner(config);
	}

	return true;
}

static bool pgfsk_link_process_config_request(k_timeout_t timeout)
{
	struct pgfsk_link_config_request request;

	if (k_msgq_get(&g_pgfsk_config_queue, &request, timeout) != 0) {
		return false;
	}

	g_pgfsk_config_completed_result = pgfsk_link_apply_config_internal(&request.config);
	g_pgfsk_config_completed_seq = request.seq;
	k_sem_give(&g_pgfsk_config_done_sem);
	return true;
}

bool pgfsk_link_set_config(const struct pgfsk_link_config *config)
{
	struct pgfsk_link_config_request request;
	int64_t deadline_ms;
	bool result;

	if (config == NULL) {
		return false;
	}

	k_mutex_lock(&g_pgfsk_config_lock, K_FOREVER);
	k_sem_reset(&g_pgfsk_config_done_sem);

	request = (struct pgfsk_link_config_request){
		.config = *config,
		.seq = ++g_pgfsk_config_request_seq,
	};

	k_msgq_purge(&g_pgfsk_config_queue);
	(void)k_msgq_put(&g_pgfsk_config_queue, &request, K_FOREVER);

	deadline_ms = k_uptime_get() + PGFSK_LINK_CONFIG_SET_TIMEOUT_MS;
	while (1) {
		int64_t remaining_ms = deadline_ms - k_uptime_get();

		if (remaining_ms <= 0) {
			LOG_ERR("timed out waiting for link config apply");
			k_mutex_unlock(&g_pgfsk_config_lock);
			return false;
		}

		if (k_sem_take(&g_pgfsk_config_done_sem,
			       K_MSEC((uint32_t)remaining_ms)) != 0) {
			LOG_ERR("timed out waiting for link config apply");
			k_mutex_unlock(&g_pgfsk_config_lock);
			return false;
		}

		if (g_pgfsk_config_completed_seq == request.seq) {
			result = g_pgfsk_config_completed_result;
			k_mutex_unlock(&g_pgfsk_config_lock);
			return result;
		}
	}
}

bool pgfsk_link_stop(void)
{
	return pgfsk_link_set_config(&(struct pgfsk_link_config){
		.enabled = false,
		.local_device_role = DEVICE_ROLE_HEADSET,
	});
}

bool pgfsk_link_is_enabled(void)
{
	return pgfsk_link_get_config().enabled;
}

enum pgfsk_link_state pgfsk_link_get_state(void)
{
	k_spinlock_key_t key = k_spin_lock(&g_link.lock);
	enum pgfsk_link_state state = g_link.service_state;

	k_spin_unlock(&g_link.lock, key);
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

	k_spinlock_key_t key = k_spin_lock(&g_link.lock);
	stats = g_link.stats;
	state = g_link.service_state;
	in_service_since_cyc = g_link.in_service_since_cyc;
	k_spin_unlock(&g_link.lock, key);

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
	if (frame == NULL || frame->len > PGFSK_PAYLOAD_MAX_LEN ||
	    !pgfsk_link_is_enabled()) {
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
	enum { POLL_RADIO = 0, POLL_CONFIG = 1 };
	struct k_poll_event events[2] = {
		[POLL_RADIO]  = K_POLL_EVENT_INITIALIZER(K_POLL_TYPE_MSGQ_DATA_AVAILABLE, K_POLL_MODE_NOTIFY_ONLY, pgfsk_hw_event_msgq()),
		[POLL_CONFIG] = K_POLL_EVENT_INITIALIZER(K_POLL_TYPE_MSGQ_DATA_AVAILABLE, K_POLL_MODE_NOTIFY_ONLY, &g_pgfsk_config_queue),
	};

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	pgfsk_hw_init();
	k_msgq_purge(&g_pgfsk_tx_queue);
	k_msgq_purge(&g_pgfsk_rx_queue);

	while (1) {
		struct pgfsk_link_config config = pgfsk_link_get_config();
		if (!config.enabled) {
			(void)pgfsk_link_process_config_request(K_FOREVER);
			continue;
		}

		events[POLL_RADIO].state = K_POLL_STATE_NOT_READY;
		events[POLL_CONFIG].state = K_POLL_STATE_NOT_READY;

		(void)k_poll(events, ARRAY_SIZE(events), K_FOREVER);

		if (events[POLL_CONFIG].state == K_POLL_STATE_MSGQ_DATA_AVAILABLE) {
			while (pgfsk_link_process_config_request(K_NO_WAIT));
			continue;
		}

		if (events[POLL_RADIO].state == K_POLL_STATE_MSGQ_DATA_AVAILABLE) {
			struct pgfsk_hw_event event;
			while (pgfsk_hw_dequeue_event(&event, K_NO_WAIT)) {
				pgfsk_link_handle_radio_event(&event);
			}
		}
	}
}

K_THREAD_DEFINE(pgfsk_link_thread_id,
		PGFSK_LINK_THREAD_STACK_SIZE,
		pgfsk_link_thread,
		NULL, NULL, NULL,
		PGFSK_LINK_THREAD_PRIORITY, 0, 0);
