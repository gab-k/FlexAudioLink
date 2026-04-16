#include "prop_gfsk/link.h"

#include <string.h>

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(prop_gfsk_link, CONFIG_LOG_DEFAULT_LEVEL);

#define PROP_GFSK_LINK_THREAD_STACK_SIZE       2048
#define PROP_GFSK_LINK_THREAD_PRIORITY         6
#define PROP_GFSK_LINK_QUEUE_DEPTH             4
#define PROP_GFSK_LINK_IFS_US                  80U
#define PROP_GFSK_LINK_MAX_PACKET_AIRTIME_US   550U
#define PROP_GFSK_LINK_SYNC_LOSS_TURNS         8U
#define PROP_GFSK_LINK_FIXED_TICK_US           1000U
#define PROP_GFSK_LINK_RANDOM_TICKS_US         1000U
#define PROP_GFSK_LINK_CONFIG_SET_TIMEOUT_MS   1000

enum prop_gfsk_internal_state {
	PROP_GFSK_STATE_IN_RX = 0,
	PROP_GFSK_STATE_PREPARE_PACKET,
	PROP_GFSK_STATE_IN_TX,
	PROP_GFSK_STATE_LISTEN,
};

struct link_timing {
	uint32_t last_prepare_us;
	uint32_t max_prepare_us;
	uint32_t last_turnaround_us;
	uint32_t max_turnaround_us;
};

struct link_stats {
	uint32_t packets_lost_in_service;
	uint32_t loss_burst_1_count;
	uint32_t loss_burst_2_count;
	uint32_t loss_burst_3_4_count;
	uint32_t loss_burst_5_plus_count;
	uint32_t max_loss_burst_len;
	uint32_t outage_count;
	uint32_t tx_intended_count;
	uint32_t tx_missed_deadline_count;
	uint64_t time_in_service_us;
};

struct prop_gfsk_link_runtime {
	struct prop_gfsk_link_config config;
	enum prop_gfsk_link_state service_state;
	enum prop_gfsk_internal_state state;
	struct link_stats stats;
	struct link_timing timing;
	uint32_t tx_ready_tick;
	uint32_t rx_deadline_tick;
	uint32_t last_anchor_tick;
	uint8_t consecutive_rx_misses;
	uint64_t in_service_since_cyc;
	uint16_t next_tx_seq;
	uint16_t last_rx_seq;
	bool have_last_rx_seq;
	bool tx_packet_ready;
	struct prop_gfsk_packet prepared_tx_packet;
	struct k_spinlock lock;
};

struct prop_gfsk_link_config_request {
	struct prop_gfsk_link_config config;
	uint32_t seq;
};

static struct prop_gfsk_link_runtime g_link = {
	.config = {
		.enabled = false,
		.local_device_role = DEVICE_ROLE_HEADSET,
	},
	.service_state = PROP_GFSK_LINK_STATE_DISABLED,
	.state = PROP_GFSK_STATE_LISTEN,
};

static uint32_t g_prop_gfsk_config_request_seq;
static uint32_t g_prop_gfsk_config_completed_seq;
static bool g_prop_gfsk_config_completed_result;
static uint32_t g_prop_gfsk_probe_prng_state;

K_MSGQ_DEFINE(g_prop_gfsk_tx_queue, sizeof(struct prop_gfsk_frame),PROP_GFSK_LINK_QUEUE_DEPTH, 4);
K_MSGQ_DEFINE(g_prop_gfsk_rx_queue, sizeof(struct prop_gfsk_frame),PROP_GFSK_LINK_QUEUE_DEPTH, 4);
K_MSGQ_DEFINE(g_prop_gfsk_config_queue,sizeof(struct prop_gfsk_link_config_request), 1, 4);
K_MUTEX_DEFINE(g_prop_gfsk_config_lock);
K_SEM_DEFINE(g_prop_gfsk_config_done_sem, 0, 1);

static inline int32_t tick_diff(uint32_t a, uint32_t b)
{
	return (int32_t)(a - b);
}

static inline bool tick_reached(uint32_t now, uint32_t deadline)
{
	return tick_diff(now, deadline) >= 0;
}

static bool prop_gfsk_link_seq_gap_from_expected(uint16_t expected, uint16_t seq, uint16_t *gap)
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

static struct prop_gfsk_link_config prop_gfsk_link_get_config(void)
{
	k_spinlock_key_t key = k_spin_lock(&g_link.lock);
	struct prop_gfsk_link_config config = g_link.config;

	k_spin_unlock(&g_link.lock, key);
	return config;
}

static void prop_gfsk_link_seed_probe_prng(void)
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

	g_prop_gfsk_probe_prng_state = seed;
}

static uint32_t prop_gfsk_link_random_probe_jitter(void)
{
	uint32_t x;

	if (PROP_GFSK_LINK_RANDOM_TICKS_US == 0U) {
		return 0U;
	}

	if (g_prop_gfsk_probe_prng_state == 0U) {
		prop_gfsk_link_seed_probe_prng();
	}

	x = g_prop_gfsk_probe_prng_state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	if (x == 0U) {
		x = 0x9e3779b9U;
	}

	g_prop_gfsk_probe_prng_state = x;
	return x % (PROP_GFSK_LINK_RANDOM_TICKS_US + 1U);
}

static void prop_gfsk_link_record_loss_burst(uint32_t burst_len)
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

static void prop_gfsk_link_note_in_service_time(uint64_t now_cyc)
{
	if (g_link.service_state != PROP_GFSK_LINK_STATE_IN_SERVICE ||
	    g_link.in_service_since_cyc == 0U) {
		return;
	}

	if (now_cyc > g_link.in_service_since_cyc) {
		g_link.stats.time_in_service_us +=
			k_cyc_to_us_floor64(now_cyc - g_link.in_service_since_cyc);
	}

	g_link.in_service_since_cyc = 0U;
}

static void prop_gfsk_link_enter_in_service(void)
{
	if (g_link.service_state == PROP_GFSK_LINK_STATE_IN_SERVICE) {
		return;
	}

	g_link.service_state = PROP_GFSK_LINK_STATE_IN_SERVICE;
	g_link.consecutive_rx_misses = 0U;
	g_link.in_service_since_cyc = k_cycle_get_64();
	LOG_INF("service established");
}

static void prop_gfsk_link_enter_no_service(uint32_t now_tick)
{
	bool was_in_service = (g_link.service_state ==
			       PROP_GFSK_LINK_STATE_IN_SERVICE);

	if (was_in_service) {
		prop_gfsk_link_note_in_service_time(k_cycle_get_64());
		g_link.stats.outage_count++;
	}

	g_link.service_state = PROP_GFSK_LINK_STATE_NO_SERVICE;
	g_link.state = PROP_GFSK_STATE_LISTEN;
	g_link.consecutive_rx_misses = 0U;
	g_link.have_last_rx_seq = false;
	g_link.tx_packet_ready = false;
	g_link.rx_deadline_tick = now_tick +
		PROP_GFSK_LINK_FIXED_TICK_US +
		prop_gfsk_link_random_probe_jitter();

	if (was_in_service) {
		LOG_WRN("service outage (listen timeouts)");
	}
}

static void prop_gfsk_link_record_in_service_rx(uint16_t seq)
{
	if (g_link.have_last_rx_seq) {
		uint16_t expected = g_link.last_rx_seq + 1U;
		uint16_t gap;

		if (prop_gfsk_link_seq_gap_from_expected(expected, seq, &gap) &&
		    gap > 0U) {
			g_link.stats.packets_lost_in_service += gap;
			prop_gfsk_link_record_loss_burst(gap);
		}
	}

	g_link.last_rx_seq = seq;
	g_link.have_last_rx_seq = true;
}

static void prop_gfsk_link_queue_rx_frame(const struct prop_gfsk_radio_event *event)
{
	struct prop_gfsk_frame frame;
	const struct prop_gfsk_packet *packet;

	if (event == NULL) {
		return;
	}

	packet = &event->packet;
	if (packet->length < PROP_GFSK_PACKET_METADATA_LEN ||
	    packet->payload_len > PROP_GFSK_PAYLOAD_LEN ||
	    packet->payload_len >
		    (packet->length - PROP_GFSK_PACKET_METADATA_LEN)) {
		return;
	}

	frame = (struct prop_gfsk_frame){
		.seq = packet->seq,
		.len = packet->payload_len,
		.rssi_dbm = event->rssi_dbm,
	};
	memcpy(frame.payload, packet->data, frame.len);
	(void)k_msgq_put(&g_prop_gfsk_rx_queue, &frame, K_NO_WAIT);
}

static void prop_gfsk_link_prepare_tx_packet(struct prop_gfsk_packet *packet, uint16_t seq)
{
	struct prop_gfsk_frame frame;
	bool has_payload;

	if (packet == NULL) {
		return;
	}

	has_payload = (k_msgq_get(&g_prop_gfsk_tx_queue, &frame, K_NO_WAIT) == 0);

	memset(packet, 0, sizeof(*packet));
	packet->length = PROP_GFSK_PACKET_METADATA_LEN;
	packet->seq = seq;

	if (!has_payload) {
		packet->payload_len = 0U;
		return;
	}

	packet->payload_len = frame.len;
	packet->length = PROP_GFSK_PACKET_METADATA_LEN + packet->payload_len;
	memcpy(packet->data, frame.payload, frame.len);
}

static void prop_gfsk_link_transition_to_prepare(uint32_t anchor_tick)
{
	uint32_t t0, t1, prepare_us;

	g_link.state = PROP_GFSK_STATE_PREPARE_PACKET;
	g_link.tx_ready_tick = anchor_tick + PROP_GFSK_LINK_IFS_US;
	g_link.last_anchor_tick = anchor_tick;

	t0 = prop_gfsk_radio_hw_get_tick();
	prop_gfsk_link_prepare_tx_packet(&g_link.prepared_tx_packet, g_link.next_tx_seq);
	t1 = prop_gfsk_radio_hw_get_tick();

	prepare_us = t1 - t0;
	g_link.timing.last_prepare_us = prepare_us;
	if (prepare_us > g_link.timing.max_prepare_us) {
		g_link.timing.max_prepare_us = prepare_us;
	}

	g_link.tx_packet_ready = true;
}

static void prop_gfsk_link_handle_rx_address(const struct prop_gfsk_radio_event *event)
{
	if (event == NULL || g_link.state != PROP_GFSK_STATE_LISTEN) {
		return;
	}

	g_link.state = PROP_GFSK_STATE_IN_RX;
	g_link.rx_deadline_tick = event->tick +
		PROP_GFSK_LINK_MAX_PACKET_AIRTIME_US;
}

static void prop_gfsk_link_handle_rx_ok(const struct prop_gfsk_radio_event *event)
{
	if (event == NULL ||
	    (g_link.state != PROP_GFSK_STATE_LISTEN &&
	     g_link.state != PROP_GFSK_STATE_IN_RX)) {
		return;
	}

	prop_gfsk_link_queue_rx_frame(event);
	g_link.consecutive_rx_misses = 0U;
	prop_gfsk_link_enter_in_service();
	prop_gfsk_link_record_in_service_rx(event->packet.seq);
	prop_gfsk_link_transition_to_prepare(event->tick);
}

static void prop_gfsk_link_handle_rx_bad_anchor(uint32_t anchor_tick)
{
	if (g_link.state != PROP_GFSK_STATE_LISTEN &&
	    g_link.state != PROP_GFSK_STATE_IN_RX) {
		return;
	}

	g_link.consecutive_rx_misses = 0U;
	prop_gfsk_link_transition_to_prepare(anchor_tick);
}

static void prop_gfsk_link_handle_tx_end(const struct prop_gfsk_radio_event *event)
{
	if (event == NULL || g_link.state != PROP_GFSK_STATE_IN_TX) {
		return;
	}

	g_link.next_tx_seq++;
	g_link.tx_packet_ready = false;
	g_link.state = PROP_GFSK_STATE_LISTEN;
	if (g_link.service_state == PROP_GFSK_LINK_STATE_IN_SERVICE) {
		g_link.rx_deadline_tick = event->tick +
			PROP_GFSK_LINK_MAX_PACKET_AIRTIME_US;
	} else {
		g_link.rx_deadline_tick = event->tick +
			PROP_GFSK_LINK_MAX_PACKET_AIRTIME_US +
			prop_gfsk_link_random_probe_jitter();
	}
}

static void prop_gfsk_link_handle_radio_event(const struct prop_gfsk_radio_event *event)
{
	if (event == NULL) {
		return;
	}

	switch (event->type) {
	case PROP_GFSK_RADIO_EVENT_RX_ADDRESS:
		prop_gfsk_link_handle_rx_address(event);
		break;
	case PROP_GFSK_RADIO_EVENT_RX_OK:
		prop_gfsk_link_handle_rx_ok(event);
		break;
	case PROP_GFSK_RADIO_EVENT_RX_BAD:
		prop_gfsk_link_handle_rx_bad_anchor(event->tick);
		break;
	case PROP_GFSK_RADIO_EVENT_TX_END:
		prop_gfsk_link_handle_tx_end(event);
		break;
	default:
		break;
	}
}

static void prop_gfsk_link_handle_in_rx_timeout(void)
{
	if (g_link.state != PROP_GFSK_STATE_IN_RX) {
		return;
	}

	prop_gfsk_link_handle_rx_bad_anchor(g_link.rx_deadline_tick);
}

static void prop_gfsk_link_handle_listen_timeout(uint32_t now_tick)
{
	if (g_link.state != PROP_GFSK_STATE_LISTEN) {
		return;
	}

	if (g_link.service_state == PROP_GFSK_LINK_STATE_IN_SERVICE) {
		g_link.consecutive_rx_misses++;
		if (g_link.consecutive_rx_misses >=
		    PROP_GFSK_LINK_SYNC_LOSS_TURNS) {
			prop_gfsk_link_enter_no_service(now_tick);
			return;
		}
	}

	prop_gfsk_link_transition_to_prepare(g_link.rx_deadline_tick);
}

static void prop_gfsk_link_commit_prepared_tx(uint32_t now_tick)
{
	uint32_t turnaround_us;

	if (g_link.state != PROP_GFSK_STATE_PREPARE_PACKET ||
	    !g_link.tx_packet_ready) {
		return;
	}

	turnaround_us = now_tick - g_link.last_anchor_tick;
	g_link.timing.last_turnaround_us = turnaround_us;
	if (turnaround_us > g_link.timing.max_turnaround_us) {
		g_link.timing.max_turnaround_us = turnaround_us;
	}

	g_link.state = PROP_GFSK_STATE_IN_TX;
	g_link.stats.tx_intended_count++;

	if (prop_gfsk_radio_hw_start_tx(&g_link.prepared_tx_packet)) {
		return;
	}

	g_link.stats.tx_missed_deadline_count++;
	g_link.state = PROP_GFSK_STATE_LISTEN;
	g_link.tx_packet_ready = false;
	g_link.rx_deadline_tick = now_tick + (2U * PROP_GFSK_LINK_MAX_PACKET_AIRTIME_US);
	(void)prop_gfsk_radio_hw_start_listen();
}

static void prop_gfsk_link_run_state_machine(void)
{
	while (g_link.config.enabled) {
		uint32_t now_tick = prop_gfsk_radio_hw_get_tick();

		switch (g_link.state) {
		case PROP_GFSK_STATE_IN_RX:
			if (tick_reached(now_tick, g_link.rx_deadline_tick)) {
				prop_gfsk_link_handle_in_rx_timeout();
				continue;
			}
			return;
		case PROP_GFSK_STATE_PREPARE_PACKET:
			if (g_link.tx_packet_ready && tick_reached(now_tick, g_link.tx_ready_tick)) {
				prop_gfsk_link_commit_prepared_tx(now_tick);
				continue;
			}
			return;
		case PROP_GFSK_STATE_LISTEN:
			if (tick_reached(now_tick, g_link.rx_deadline_tick)) {
				prop_gfsk_link_handle_listen_timeout(now_tick);
				continue;
			}
			return;
		case PROP_GFSK_STATE_IN_TX:
		default:
			return;
		}
	}
}

static k_timeout_t prop_gfsk_link_get_poll_timeout(void)
{
	uint32_t deadline_tick;
	uint32_t now_tick;
	int32_t delta_us;

	switch (g_link.state) {
	case PROP_GFSK_STATE_IN_RX:
	case PROP_GFSK_STATE_LISTEN:
		deadline_tick = g_link.rx_deadline_tick;
		break;
	case PROP_GFSK_STATE_PREPARE_PACKET:
		deadline_tick = g_link.tx_ready_tick;
		break;
	case PROP_GFSK_STATE_IN_TX:
	default:
		return K_FOREVER;
	}

	now_tick = prop_gfsk_radio_hw_get_tick();
	delta_us = tick_diff(deadline_tick, now_tick);
	if (delta_us <= 0) {
		return K_NO_WAIT;
	}

	return K_USEC((uint32_t)delta_us);
}

static void prop_gfsk_link_print_banner(const struct prop_gfsk_link_config *config)
{
	const char *role;

	if (config == NULL || !config->enabled) {
		return;
	}

	role = (config->local_device_role == DEVICE_ROLE_DONGLE) ? "dongle" : "headset";
	LOG_INF("=== FlexLink GFSK Link ===");
	LOG_INF("Role: %s", role);
	LOG_INF("Frequency: %u MHz", PROP_GFSK_RADIO_FREQUENCY_MHZ);
	LOG_INF("Rate: 4 Mbps");
	LOG_INF("TX Power: +8 dBm");
	LOG_INF("IFS: %u us", PROP_GFSK_LINK_IFS_US);
	LOG_INF("Max packet airtime: %u us", PROP_GFSK_LINK_MAX_PACKET_AIRTIME_US);
	LOG_INF("Sync loss turns: %u", PROP_GFSK_LINK_SYNC_LOSS_TURNS);
	LOG_INF("Probe delay: %u..%u us", 
			PROP_GFSK_LINK_FIXED_TICK_US, PROP_GFSK_LINK_FIXED_TICK_US + 
			PROP_GFSK_LINK_RANDOM_TICKS_US);
	LOG_INF("Dongle addr: 0x%08lX", PROP_GFSK_RADIO_ADDR_DONGLE);
	LOG_INF("Headset addr: 0x%08lX", PROP_GFSK_RADIO_ADDR_HEADSET);
}

static bool prop_gfsk_link_apply_config_internal(const struct prop_gfsk_link_config *config)
{
	struct prop_gfsk_link_config prev;
	uint32_t now_tick;
	bool changed;

	if (config == NULL) {
		return false;
	}

	if (config->local_device_role != DEVICE_ROLE_DONGLE &&
	    config->local_device_role != DEVICE_ROLE_HEADSET) {
		return false;
	}

	prev = prop_gfsk_link_get_config();

	prop_gfsk_radio_hw_stop();
	k_msgq_purge(&g_prop_gfsk_tx_queue);
	k_msgq_purge(&g_prop_gfsk_rx_queue);

	k_spinlock_key_t key = k_spin_lock(&g_link.lock);

	g_link.config = *config;
	g_link.service_state = config->enabled ?
		PROP_GFSK_LINK_STATE_NO_SERVICE :
		PROP_GFSK_LINK_STATE_DISABLED;
	g_link.state = PROP_GFSK_STATE_LISTEN;
	memset(&g_link.stats, 0, sizeof(g_link.stats));
	g_link.tx_ready_tick = 0U;
	g_link.rx_deadline_tick = 0U;
	g_link.consecutive_rx_misses = 0U;
	g_link.in_service_since_cyc = 0U;
	g_link.next_tx_seq = 0U;
	g_link.last_rx_seq = 0U;
	g_link.have_last_rx_seq = false;
	g_link.tx_packet_ready = false;
	memset(&g_link.prepared_tx_packet, 0, sizeof(g_link.prepared_tx_packet));

	k_spin_unlock(&g_link.lock, key);

	if (!config->enabled) {
		return true;
	}

	prop_gfsk_radio_hw_set_role(config->local_device_role);
	prop_gfsk_radio_hw_start();
	prop_gfsk_radio_hw_reset_stats();

	now_tick = prop_gfsk_radio_hw_get_tick();

	key = k_spin_lock(&g_link.lock);
	g_link.rx_deadline_tick = now_tick +
		PROP_GFSK_LINK_FIXED_TICK_US +
		prop_gfsk_link_random_probe_jitter();
	k_spin_unlock(&g_link.lock, key);

	if (!prop_gfsk_radio_hw_start_listen()) {
		prop_gfsk_radio_hw_stop();
		key = k_spin_lock(&g_link.lock);
		g_link.config.enabled = false;
		g_link.service_state = PROP_GFSK_LINK_STATE_DISABLED;
		g_link.rx_deadline_tick = 0U;
		k_spin_unlock(&g_link.lock, key);
		return false;
	}

	changed = (!prev.enabled ||
		   prev.local_device_role != config->local_device_role);
	if (changed) {
		prop_gfsk_link_print_banner(config);
	}

	return true;
}

static bool prop_gfsk_link_process_config_request(k_timeout_t timeout)
{
	struct prop_gfsk_link_config_request request;

	if (k_msgq_get(&g_prop_gfsk_config_queue, &request, timeout) != 0) {
		return false;
	}

	g_prop_gfsk_config_completed_result = prop_gfsk_link_apply_config_internal(&request.config);
	g_prop_gfsk_config_completed_seq = request.seq;
	k_sem_give(&g_prop_gfsk_config_done_sem);
	return true;
}

bool prop_gfsk_link_set_config(const struct prop_gfsk_link_config *config)
{
	struct prop_gfsk_link_config_request request;
	int64_t deadline_ms;
	bool result;

	if (config == NULL) {
		return false;
	}

	k_mutex_lock(&g_prop_gfsk_config_lock, K_FOREVER);
	k_sem_reset(&g_prop_gfsk_config_done_sem);

	request = (struct prop_gfsk_link_config_request){
		.config = *config,
		.seq = ++g_prop_gfsk_config_request_seq,
	};

	k_msgq_purge(&g_prop_gfsk_config_queue);
	(void)k_msgq_put(&g_prop_gfsk_config_queue, &request, K_FOREVER);

	deadline_ms = k_uptime_get() + PROP_GFSK_LINK_CONFIG_SET_TIMEOUT_MS;
	while (1) {
		int64_t remaining_ms = deadline_ms - k_uptime_get();

		if (remaining_ms <= 0) {
			LOG_ERR("timed out waiting for link config apply");
			k_mutex_unlock(&g_prop_gfsk_config_lock);
			return false;
		}

		if (k_sem_take(&g_prop_gfsk_config_done_sem,
			       K_MSEC((uint32_t)remaining_ms)) != 0) {
			LOG_ERR("timed out waiting for link config apply");
			k_mutex_unlock(&g_prop_gfsk_config_lock);
			return false;
		}

		if (g_prop_gfsk_config_completed_seq == request.seq) {
			result = g_prop_gfsk_config_completed_result;
			k_mutex_unlock(&g_prop_gfsk_config_lock);
			return result;
		}
	}
}

bool prop_gfsk_link_stop(void)
{
	return prop_gfsk_link_set_config(&(struct prop_gfsk_link_config){
		.enabled = false,
		.local_device_role = DEVICE_ROLE_HEADSET,
	});
}

bool prop_gfsk_link_is_enabled(void)
{
	return prop_gfsk_link_get_config().enabled;
}

enum prop_gfsk_link_state prop_gfsk_link_get_state(void)
{
	k_spinlock_key_t key = k_spin_lock(&g_link.lock);
	enum prop_gfsk_link_state state = g_link.service_state;

	k_spin_unlock(&g_link.lock, key);
	return state;
}

void prop_gfsk_link_get_report(struct prop_gfsk_link_report *report)
{
	struct prop_gfsk_hw_stats hw_stats;
	struct link_stats stats;
	struct link_timing timing;
	enum prop_gfsk_link_state state;
	uint64_t in_service_since_cyc;

	if (report == NULL) {
		return;
	}

	prop_gfsk_radio_hw_get_stats(&hw_stats);

	k_spinlock_key_t key = k_spin_lock(&g_link.lock);
	stats = g_link.stats;
	timing = g_link.timing;
	state = g_link.service_state;
	in_service_since_cyc = g_link.in_service_since_cyc;
	k_spin_unlock(&g_link.lock, key);

	if (state == PROP_GFSK_LINK_STATE_IN_SERVICE &&
	    in_service_since_cyc != 0U) {
		stats.time_in_service_us += k_cyc_to_us_floor64(
			k_cycle_get_64() - in_service_since_cyc);
	}

	*report = (struct prop_gfsk_link_report){
		.packets_tx = hw_stats.packets_tx,
		.packets_rx = hw_stats.packets_rx,
		.packets_lost_in_service = stats.packets_lost_in_service,
		.loss_burst_1_count = stats.loss_burst_1_count,
		.loss_burst_2_count = stats.loss_burst_2_count,
		.loss_burst_3_4_count = stats.loss_burst_3_4_count,
		.loss_burst_5_plus_count = stats.loss_burst_5_plus_count,
		.max_loss_burst_len = stats.max_loss_burst_len,
		.crc_error_count = hw_stats.crc_errors,
		.tx_intended_count = stats.tx_intended_count,
		.tx_missed_deadline_count = stats.tx_missed_deadline_count,
		.outage_count = stats.outage_count,
		.time_in_service_us = stats.time_in_service_us,
		.last_prepare_us = timing.last_prepare_us,
		.max_prepare_us = timing.max_prepare_us,
		.last_turnaround_us = timing.last_turnaround_us,
		.max_turnaround_us = timing.max_turnaround_us,
		.last_rssi_dbm = hw_stats.last_rssi_dbm,
		.state = state,
	};
}

bool prop_gfsk_link_tx_enqueue(const struct prop_gfsk_frame *frame, k_timeout_t timeout)
{
	if (frame == NULL || frame->len > PROP_GFSK_PAYLOAD_LEN ||
	    !prop_gfsk_link_is_enabled()) {
		return false;
	}

	return k_msgq_put(&g_prop_gfsk_tx_queue, frame, timeout) == 0;
}

bool prop_gfsk_link_rx_dequeue(struct prop_gfsk_frame *frame, k_timeout_t timeout)
{
	if (frame == NULL) {
		return false;
	}

	return k_msgq_get(&g_prop_gfsk_rx_queue, frame, timeout) == 0;
}

static void prop_gfsk_link_thread(void *arg1, void *arg2, void *arg3)
{
	enum { POLL_RADIO = 0, POLL_CONFIG = 1 };
	struct k_poll_event events[2] = {
		[POLL_RADIO]  = K_POLL_EVENT_INITIALIZER(K_POLL_TYPE_MSGQ_DATA_AVAILABLE, K_POLL_MODE_NOTIFY_ONLY, prop_gfsk_radio_hw_event_msgq()),
		[POLL_CONFIG] = K_POLL_EVENT_INITIALIZER(K_POLL_TYPE_MSGQ_DATA_AVAILABLE, K_POLL_MODE_NOTIFY_ONLY, &g_prop_gfsk_config_queue),
	};

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	prop_gfsk_radio_hw_init();
	k_msgq_purge(&g_prop_gfsk_tx_queue);
	k_msgq_purge(&g_prop_gfsk_rx_queue);

	while (1) {
		struct prop_gfsk_link_config config = prop_gfsk_link_get_config();
		if (!config.enabled) {
			(void)prop_gfsk_link_process_config_request(K_FOREVER);
			continue;
		}

		events[POLL_RADIO].state = K_POLL_STATE_NOT_READY;
		events[POLL_CONFIG].state = K_POLL_STATE_NOT_READY;

		(void)k_poll(events, ARRAY_SIZE(events), prop_gfsk_link_get_poll_timeout());

		if (events[POLL_CONFIG].state == K_POLL_STATE_MSGQ_DATA_AVAILABLE) {
			while (prop_gfsk_link_process_config_request(K_NO_WAIT));
			continue;
		}

		if (events[POLL_RADIO].state == K_POLL_STATE_MSGQ_DATA_AVAILABLE) {
			struct prop_gfsk_radio_event event;
			while (prop_gfsk_radio_hw_dequeue_event(&event, K_NO_WAIT)) {
				prop_gfsk_link_handle_radio_event(&event);
			}
		}

		prop_gfsk_link_run_state_machine();
	}
}

K_THREAD_DEFINE(prop_gfsk_link_thread_id,
		PROP_GFSK_LINK_THREAD_STACK_SIZE,
		prop_gfsk_link_thread,
		NULL, NULL, NULL,
		PROP_GFSK_LINK_THREAD_PRIORITY, 0, 0);
