#include "prop_gfsk/link.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define PROP_GFSK_LINK_THREAD_STACK_SIZE   2048
#define PROP_GFSK_LINK_THREAD_PRIORITY     6
#define PROP_GFSK_LINK_QUEUE_DEPTH         4
#define PROP_GFSK_LINK_DEFAULT_DONGLE_TX_SLOT_US  1000U
#define PROP_GFSK_LINK_DEFAULT_HEADSET_TX_SLOT_US 1000U
/* Service-outage timeout: after this many missed expected peer RX windows,
 * the active stream is considered out of service. */
#define PROP_GFSK_LINK_SYNC_LOSS_FRAMES    8U
#define PROP_GFSK_LINK_CONFIG_SET_TIMEOUT  K_MSEC(1000)

enum prop_gfsk_tx_tick_source {
	PROP_GFSK_TX_TICK_SRC_NONE = 0,
	PROP_GFSK_TX_TICK_SRC_FALLBACK,
	PROP_GFSK_TX_TICK_SRC_RX_SYNC,
};

struct link_stats {
	uint32_t packets_lost_in_service;
	uint32_t loss_burst_count;
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
	enum prop_gfsk_link_state state;
	struct link_stats stats;
	uint16_t tx_seq;
	struct {
		uint16_t last_rx_seq;
		bool have_last_rx_seq;
		uint64_t in_service_since_cyc;
		uint32_t next_tx_tick;
		uint16_t pending_tx_seq;
		bool pending_tx_seq_valid;
		bool tx_armed;
		enum prop_gfsk_tx_tick_source next_tx_tick_src;
		uint8_t consecutive_missed_peer_rx_frames;
	} slot_sync;
	struct k_spinlock lock;
};

struct prop_gfsk_link_config_set_request {
	struct prop_gfsk_link_config config;
	struct k_sem *done;
	bool *result;
};

static struct prop_gfsk_link_runtime g_link = {
	.config = {
		.enabled            = false,
		.local_device_role  = DEVICE_ROLE_HEADSET,
		.dongle_tx_slot_us  = PROP_GFSK_LINK_DEFAULT_DONGLE_TX_SLOT_US,
		.headset_tx_slot_us = PROP_GFSK_LINK_DEFAULT_HEADSET_TX_SLOT_US,
	},
	.state = PROP_GFSK_LINK_STATE_DISABLED,
};

K_MSGQ_DEFINE(g_prop_gfsk_tx_queue, sizeof(struct prop_gfsk_frame),
	      PROP_GFSK_LINK_QUEUE_DEPTH, 4);
K_MSGQ_DEFINE(g_prop_gfsk_rx_queue, sizeof(struct prop_gfsk_frame),
	      PROP_GFSK_LINK_QUEUE_DEPTH, 4);
K_MSGQ_DEFINE(g_prop_gfsk_config_queue,
	      sizeof(struct prop_gfsk_link_config_set_request), 1, 4);
K_MUTEX_DEFINE(g_prop_gfsk_config_set_lock);

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

static struct prop_gfsk_link_config prop_gfsk_link_get_config(void)
{
	k_spinlock_key_t key = k_spin_lock(&g_link.lock);
	struct prop_gfsk_link_config config = g_link.config;

	k_spin_unlock(&g_link.lock, key);
	return config;
}

static bool prop_gfsk_link_schedule_tx(uint32_t tx_tick, enum prop_gfsk_tx_tick_source source);

/* A loss burst is a contiguous recovered sequence gap. This intentionally
 * shares the same source of truth as packets_lost_in_service so the histogram
 * always matches the visible `lost` counter. */
static void prop_gfsk_link_record_loss_burst(uint32_t burst_len)
{
	if (burst_len == 0U) {
		return;
	}

	g_link.stats.loss_burst_count++;
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
	if (g_link.state != PROP_GFSK_LINK_STATE_IN_SERVICE ||
	    g_link.slot_sync.in_service_since_cyc == 0U) {
		return;
	}

	if (now_cyc > g_link.slot_sync.in_service_since_cyc) {
		g_link.stats.time_in_service_us +=
			k_cyc_to_us_floor64(now_cyc -
					    g_link.slot_sync.in_service_since_cyc);
	}

	g_link.slot_sync.in_service_since_cyc = 0U;
}

static void prop_gfsk_link_set_no_service_locked(void)
{
	prop_gfsk_link_note_in_service_time(k_cycle_get_64());
	if (g_link.state == PROP_GFSK_LINK_STATE_IN_SERVICE) {
		g_link.stats.outage_count++;
	}
	g_link.state = g_link.config.enabled ? PROP_GFSK_LINK_STATE_NO_SERVICE
					     : PROP_GFSK_LINK_STATE_DISABLED;
	g_link.slot_sync.have_last_rx_seq = false;
	g_link.slot_sync.consecutive_missed_peer_rx_frames = 0U;
}

static void prop_gfsk_link_set_in_service_locked(void)
{
	if (g_link.state != PROP_GFSK_LINK_STATE_IN_SERVICE) {
		g_link.slot_sync.in_service_since_cyc = k_cycle_get_64();
	}
	g_link.state = PROP_GFSK_LINK_STATE_IN_SERVICE;
}

static bool prop_gfsk_link_seq_gap_from_expected(uint16_t expected, uint16_t seq,
						 uint16_t *gap)
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

static k_timeout_t prop_gfsk_link_get_in_service_poll_timeout(uint32_t frame_interval_us, uint32_t peer_tx_slot_us)
{
	uint32_t now_tick;
	uint32_t deadline_tick;
	uint32_t local_tx_slot_us;
	int32_t delta_us;

	if (g_link.state != PROP_GFSK_LINK_STATE_IN_SERVICE) {
		return K_FOREVER;
	}

	now_tick = prop_gfsk_radio_hw_get_tick();
	local_tx_slot_us = (g_link.config.local_device_role == DEVICE_ROLE_DONGLE) ?
			       		g_link.config.dongle_tx_slot_us :
			       		g_link.config.headset_tx_slot_us;
	deadline_tick = g_link.slot_sync.next_tx_tick + local_tx_slot_us;
	if (peer_tx_slot_us > PROP_GFSK_PRE_TX_OFFSET_US) {
		deadline_tick += peer_tx_slot_us - PROP_GFSK_PRE_TX_OFFSET_US;
	}

	delta_us = (int32_t)(deadline_tick - now_tick);
	if (delta_us <= 0) {
		return K_NO_WAIT;
	}

	if ((uint32_t)delta_us > frame_interval_us * PROP_GFSK_LINK_SYNC_LOSS_FRAMES) {
		return K_USEC((uint64_t)frame_interval_us * PROP_GFSK_LINK_SYNC_LOSS_FRAMES);
	}

	return K_USEC((uint32_t)delta_us);
}

static uint32_t prop_gfsk_link_get_headset_tx_tick(uint32_t dongle_rx_tick,
						   const struct prop_gfsk_link_config *config)
{
	if (config == NULL) {
		return dongle_rx_tick;
	}

	return dongle_rx_tick + config->dongle_tx_slot_us;
}

static bool prop_gfsk_link_propose_next_tx_tick(uint32_t tx_tick, enum prop_gfsk_tx_tick_source source)
{
	k_spinlock_key_t key = k_spin_lock(&g_link.lock);

	if (tx_tick == 0U || source == PROP_GFSK_TX_TICK_SRC_NONE) {
		k_spin_unlock(&g_link.lock, key);
		return false;
	}

	if (g_link.slot_sync.tx_armed &&
	    g_link.slot_sync.next_tx_tick == tx_tick) {
		if (g_link.slot_sync.next_tx_tick_src == source) {
			k_spin_unlock(&g_link.lock, key);
			return true;
		}

		if (source == PROP_GFSK_TX_TICK_SRC_FALLBACK &&
		    g_link.slot_sync.next_tx_tick_src ==
			    PROP_GFSK_TX_TICK_SRC_RX_SYNC) {
			k_spin_unlock(&g_link.lock, key);
			return true;
		}
	}

	k_spin_unlock(&g_link.lock, key);
	return prop_gfsk_link_schedule_tx(tx_tick, source);
}

static void prop_gfsk_link_record_in_service_rx(uint16_t seq)
{
	if (g_link.slot_sync.have_last_rx_seq) {
		uint16_t expected = g_link.slot_sync.last_rx_seq + 1U;
		uint16_t gap;

		if (prop_gfsk_link_seq_gap_from_expected(expected, seq, &gap) && gap > 0U) {
			g_link.stats.packets_lost_in_service += gap;
			prop_gfsk_link_record_loss_burst(gap);
		}
	}

	g_link.slot_sync.last_rx_seq = seq;
	g_link.slot_sync.have_last_rx_seq = true;
}

static void prop_gfsk_link_queue_rx_frame(const struct prop_gfsk_rx_frame *rx)
{
	struct prop_gfsk_frame frame;

	if (rx == NULL ||
	    rx->packet.length < PROP_GFSK_PACKET_METADATA_LEN ||
	    rx->packet.payload_len > PROP_GFSK_PAYLOAD_LEN ||
	    rx->packet.payload_len >
		    (rx->packet.length - PROP_GFSK_PACKET_METADATA_LEN)) {
		return;
	}

	frame = (struct prop_gfsk_frame){
		.seq      = rx->packet.seq,
		.len      = rx->packet.payload_len,
		.rssi_dbm = rx->rssi_dbm,
	};
	memcpy(frame.payload, rx->packet.data, frame.len);
	(void)k_msgq_put(&g_prop_gfsk_rx_queue, &frame, K_NO_WAIT);
}

static void prop_gfsk_link_prepare_tx_packet(struct prop_gfsk_packet *packet,
					     uint16_t seq)
{
	struct prop_gfsk_frame frame;
	bool has_payload = (k_msgq_get(&g_prop_gfsk_tx_queue, &frame, K_NO_WAIT) == 0);

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

static bool prop_gfsk_link_schedule_tx(uint32_t tx_tick, enum prop_gfsk_tx_tick_source source)
{
	struct prop_gfsk_packet tx_packet;
	const struct prop_gfsk_packet *packet = NULL;
	uint16_t seq;
	bool have_pending_tx;
	k_spinlock_key_t key = k_spin_lock(&g_link.lock);

	if (g_link.slot_sync.tx_armed &&
	    g_link.slot_sync.next_tx_tick == tx_tick &&
	    g_link.slot_sync.next_tx_tick_src == source) {
		k_spin_unlock(&g_link.lock, key);
		return true;
	}

	g_link.stats.tx_intended_count++;
	have_pending_tx = g_link.slot_sync.pending_tx_seq_valid;
	seq = have_pending_tx ? g_link.slot_sync.pending_tx_seq : g_link.tx_seq;
	k_spin_unlock(&g_link.lock, key);

	if (!have_pending_tx) {
		prop_gfsk_link_prepare_tx_packet(&tx_packet, seq);
		packet = &tx_packet;
	}

	if (!prop_gfsk_radio_hw_schedule_tx_if_possible(tx_tick, packet)) {
		key = k_spin_lock(&g_link.lock);
		g_link.stats.tx_missed_deadline_count++;
		k_spin_unlock(&g_link.lock, key);
		return false;
	}

	key = k_spin_lock(&g_link.lock);
	if (!g_link.slot_sync.pending_tx_seq_valid) {
		g_link.slot_sync.pending_tx_seq = seq;
		g_link.slot_sync.pending_tx_seq_valid = true;
	}

	g_link.slot_sync.next_tx_tick = tx_tick;
	g_link.slot_sync.next_tx_tick_src = source;
	g_link.slot_sync.tx_armed = true;
	k_spin_unlock(&g_link.lock, key);
	return true;
}

static void prop_gfsk_link_note_tx_done(void)
{
	k_spinlock_key_t key = k_spin_lock(&g_link.lock);

	if (g_link.slot_sync.pending_tx_seq_valid) {
		g_link.tx_seq = g_link.slot_sync.pending_tx_seq + 1U;
		g_link.slot_sync.pending_tx_seq_valid = false;
	}

	g_link.slot_sync.tx_armed = false;
	g_link.slot_sync.next_tx_tick_src = PROP_GFSK_TX_TICK_SRC_NONE;
	k_spin_unlock(&g_link.lock, key);
}

static void prop_gfsk_link_print_banner(const struct prop_gfsk_link_config *config)
{
	if (config == NULL || !config->enabled) {
		return;
	}

	const char *role = (config->local_device_role == DEVICE_ROLE_DONGLE)
				   ? "dongle"
				   : "headset";

	printk("\n=== FlexLink GFSK Link ===\n");
	printk("Role: %s\n", role);
	printk("Frequency: %u MHz\n", PROP_GFSK_RADIO_FREQUENCY_MHZ);
	printk("Rate: 4 Mbps\n");
	printk("TX Power: +8 dBm\n");
	printk("Dongle TX slot: %u us\n", config->dongle_tx_slot_us);
	printk("Headset TX slot: %u us\n", config->headset_tx_slot_us);
	printk("Payload: %u bytes\n", PROP_GFSK_PAYLOAD_LEN);
	printk("Dongle addr: 0x%08lX\n", PROP_GFSK_RADIO_ADDR_DONGLE);
	printk("Headset addr: 0x%08lX\n\n", PROP_GFSK_RADIO_ADDR_HEADSET);
}

static bool prop_gfsk_link_apply_config_internal(
	const struct prop_gfsk_link_config *config)
{
	if (config == NULL || config->dongle_tx_slot_us == 0U ||
	    config->headset_tx_slot_us == 0U) {
		return false;
	}

	k_msgq_purge(&g_prop_gfsk_tx_queue);
	k_msgq_purge(&g_prop_gfsk_rx_queue);

	k_spinlock_key_t key = k_spin_lock(&g_link.lock);
	struct prop_gfsk_link_config prev = g_link.config;

	g_link.config = *config;
	g_link.tx_seq = 0U;
	g_link.slot_sync.last_rx_seq = 0U;
	g_link.slot_sync.have_last_rx_seq = false;
	g_link.slot_sync.in_service_since_cyc = 0U;
	g_link.slot_sync.next_tx_tick = 0U;
	g_link.slot_sync.pending_tx_seq = 0U;
	g_link.slot_sync.pending_tx_seq_valid = false;
	g_link.slot_sync.tx_armed = false;
	g_link.slot_sync.next_tx_tick_src = PROP_GFSK_TX_TICK_SRC_NONE;
	g_link.slot_sync.consecutive_missed_peer_rx_frames = 0U;
	memset(&g_link.stats, 0, sizeof(g_link.stats));

	if (!config->enabled) {
		g_link.state = PROP_GFSK_LINK_STATE_DISABLED;
	} else {
		prop_gfsk_link_set_no_service_locked();
	}

	k_spin_unlock(&g_link.lock, key);

	if (!config->enabled) {
		prop_gfsk_radio_hw_stop();
		return true;
	}

	prop_gfsk_radio_hw_set_role(config->local_device_role);
	prop_gfsk_radio_hw_start();
	prop_gfsk_radio_hw_reset_stats();

	bool changed = !prev.enabled ||
		       prev.local_device_role != config->local_device_role ||
		       prev.dongle_tx_slot_us != config->dongle_tx_slot_us ||
		       prev.headset_tx_slot_us != config->headset_tx_slot_us;

	if (changed) {
		prop_gfsk_link_print_banner(config);
	}

	return true;
}

static bool prop_gfsk_link_process_config_request(k_timeout_t timeout)
{
	struct prop_gfsk_link_config_set_request request;

	if (k_msgq_get(&g_prop_gfsk_config_queue, &request, timeout) != 0) {
		return false;
	}

	*request.result = prop_gfsk_link_apply_config_internal(&request.config);
	k_sem_give(request.done);
	return true;
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

bool prop_gfsk_link_set_config(const struct prop_gfsk_link_config *config)
{
	if (config == NULL) {
		return false;
	}

	struct k_sem done;
	bool result;

	k_sem_init(&done, 0, 1);

	struct prop_gfsk_link_config_set_request request = {
		.config = *config,
		.done   = &done,
		.result = &result,
	};

	k_mutex_lock(&g_prop_gfsk_config_set_lock, K_FOREVER);
	k_msgq_purge(&g_prop_gfsk_config_queue);
	(void)k_msgq_put(&g_prop_gfsk_config_queue, &request, K_FOREVER);

	if (k_sem_take(&done, PROP_GFSK_LINK_CONFIG_SET_TIMEOUT) != 0) {
		printk("prop_gfsk: timed out waiting for link config apply\n");
		k_mutex_unlock(&g_prop_gfsk_config_set_lock);
		return false;
	}

	k_mutex_unlock(&g_prop_gfsk_config_set_lock);
	return result;
}

bool prop_gfsk_link_stop(void)
{
	return prop_gfsk_link_set_config(&(struct prop_gfsk_link_config){
		.enabled            = false,
		.local_device_role  = DEVICE_ROLE_HEADSET,
		.dongle_tx_slot_us  = PROP_GFSK_LINK_DEFAULT_DONGLE_TX_SLOT_US,
		.headset_tx_slot_us = PROP_GFSK_LINK_DEFAULT_HEADSET_TX_SLOT_US,
	});
}

bool prop_gfsk_link_is_enabled(void)
{
	return prop_gfsk_link_get_config().enabled;
}

enum prop_gfsk_link_state prop_gfsk_link_get_state(void)
{
	k_spinlock_key_t key = k_spin_lock(&g_link.lock);
	enum prop_gfsk_link_state state = g_link.state;

	k_spin_unlock(&g_link.lock, key);
	return state;
}

void prop_gfsk_link_get_report(struct prop_gfsk_link_report *report)
{
	if (report == NULL) {
		return;
	}

	struct prop_gfsk_hw_stats hw_stats;

	prop_gfsk_radio_hw_get_stats(&hw_stats);

	k_spinlock_key_t key = k_spin_lock(&g_link.lock);
	struct link_stats stats = g_link.stats;
	enum prop_gfsk_link_state state = g_link.state;

	if (state == PROP_GFSK_LINK_STATE_IN_SERVICE &&
	    g_link.slot_sync.in_service_since_cyc != 0U) {
		stats.time_in_service_us += k_cyc_to_us_floor64(
			k_cycle_get_64() - g_link.slot_sync.in_service_since_cyc);
	}

	k_spin_unlock(&g_link.lock, key);

	*report = (struct prop_gfsk_link_report){
		.packets_tx               = hw_stats.packets_tx,
		.packets_rx               = hw_stats.packets_rx,
		.loss_burst_count         = stats.loss_burst_count,
		.loss_burst_1_count       = stats.loss_burst_1_count,
		.loss_burst_2_count       = stats.loss_burst_2_count,
		.loss_burst_3_4_count     = stats.loss_burst_3_4_count,
		.loss_burst_5_plus_count  = stats.loss_burst_5_plus_count,
		.max_loss_burst_len       = stats.max_loss_burst_len,
		.crc_error_count          = hw_stats.crc_errors,
		.tx_intended_count        = stats.tx_intended_count,
		.tx_missed_deadline_count = stats.tx_missed_deadline_count,
		.pretx_disabled_count     = hw_stats.pretx_state_disabled_count,
		.pretx_rxidle_count       = hw_stats.pretx_state_rxidle_count,
		.pretx_rx_count           = hw_stats.pretx_state_rx_count,
		.pretx_rx_noaddr_count    = hw_stats.pretx_state_rx_noaddr_count,
		.pretx_rx_addr_count      = hw_stats.pretx_state_rx_addr_count,
		.pretx_other_count        = hw_stats.pretx_state_other_count,
		.last_rssi_dbm            = hw_stats.last_rssi_dbm,
		.packets_lost_in_service  = stats.packets_lost_in_service,
		.outage_count             = stats.outage_count,
		.time_in_service_us       = stats.time_in_service_us,
		.state                    = state,
	};
}

bool prop_gfsk_link_tx_enqueue(const struct prop_gfsk_frame *frame,
			       k_timeout_t timeout)
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

/* --------------------------------------------------------------------------
 * Link thread
 * -------------------------------------------------------------------------- */

static void prop_gfsk_link_thread(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	prop_gfsk_radio_hw_init();
	k_msgq_purge(&g_prop_gfsk_tx_queue);
	k_msgq_purge(&g_prop_gfsk_rx_queue);

	/*
	 * k_poll sources: RX queue and TX-done semaphore.
	 * Both roles wait on both; the dongle mainly acts on TX done, the
	 * headset mainly acts on RX packets.
	 */
	struct k_poll_event events[2] = {
		/* [0] RX */
		K_POLL_EVENT_INITIALIZER(K_POLL_TYPE_MSGQ_DATA_AVAILABLE,
					 				 K_POLL_MODE_NOTIFY_ONLY,
					 				 prop_gfsk_radio_hw_rx_msgq()),
		/* [1] TX done */
		K_POLL_EVENT_INITIALIZER(K_POLL_TYPE_SEM_AVAILABLE,
					 				 K_POLL_MODE_NOTIFY_ONLY,
					 				 prop_gfsk_radio_hw_tx_done_sem()), 
	};

	while (1) {
		/* Check for pending config changes before anything else. */
		if (prop_gfsk_link_process_config_request(K_NO_WAIT)) {
			continue;
		}

		struct prop_gfsk_link_config config = prop_gfsk_link_get_config();

		if (!config.enabled) {
			(void)prop_gfsk_link_process_config_request(K_MSEC(20));
			continue;
		}

		uint32_t frame_interval_us = config.dongle_tx_slot_us + config.headset_tx_slot_us;

		/*
		 * Dongle: seed the TX schedule on (re)start.
		 */
		if (config.local_device_role == DEVICE_ROLE_DONGLE) {
			bool need_seed;
			k_spinlock_key_t key = k_spin_lock(&g_link.lock);

			need_seed = (!g_link.slot_sync.tx_armed &&
				     g_link.slot_sync.next_tx_tick == 0U);
			k_spin_unlock(&g_link.lock, key);

			if (need_seed) {
				uint32_t next_tx_tick = prop_gfsk_radio_hw_get_tick() + frame_interval_us;
				(void)prop_gfsk_link_propose_next_tx_tick(next_tx_tick, PROP_GFSK_TX_TICK_SRC_FALLBACK);
			}
		}

		k_timeout_t rx_timeout = (config.local_device_role == DEVICE_ROLE_HEADSET) ?
						 prop_gfsk_link_get_in_service_poll_timeout(
							 frame_interval_us, config.dongle_tx_slot_us) :
						 prop_gfsk_link_get_in_service_poll_timeout(
							 frame_interval_us, config.headset_tx_slot_us);

		/* Reset poll states and wait. */
		events[0].state = K_POLL_STATE_NOT_READY;
		events[1].state = K_POLL_STATE_NOT_READY;
		int poll_rc = k_poll(events, ARRAY_SIZE(events), rx_timeout);

		/* --- TX done (primarily for the dongle) --- */
		if (events[1].state == K_POLL_STATE_SEM_AVAILABLE) {
			uint32_t fallback_tick = 0U;

			k_sem_take(prop_gfsk_radio_hw_tx_done_sem(), K_NO_WAIT);
			prop_gfsk_link_note_tx_done();

			if (config.local_device_role == DEVICE_ROLE_DONGLE) {
				k_spinlock_key_t key = k_spin_lock(&g_link.lock);

				if (g_link.slot_sync.next_tx_tick != 0U) {
					fallback_tick = g_link.slot_sync.next_tx_tick +
							frame_interval_us;
				}

				k_spin_unlock(&g_link.lock, key);
			} else if (config.local_device_role == DEVICE_ROLE_HEADSET) {
				k_spinlock_key_t key = k_spin_lock(&g_link.lock);

				if (g_link.state == PROP_GFSK_LINK_STATE_IN_SERVICE && g_link.slot_sync.next_tx_tick != 0U) {
					fallback_tick = g_link.slot_sync.next_tx_tick + frame_interval_us;
				}

				k_spin_unlock(&g_link.lock, key);
			}

			if (fallback_tick != 0U) {
				(void)prop_gfsk_link_propose_next_tx_tick(fallback_tick, PROP_GFSK_TX_TICK_SRC_FALLBACK);
			}
		}

		/* --- RX packet --- */
		if (events[0].state == K_POLL_STATE_MSGQ_DATA_AVAILABLE) {
			struct prop_gfsk_rx_frame rx_frame;

			while (prop_gfsk_radio_hw_rx_dequeue(&rx_frame, K_NO_WAIT)) {
				k_spinlock_key_t key = k_spin_lock(&g_link.lock);

				if (g_link.state == PROP_GFSK_LINK_STATE_IN_SERVICE) {
					prop_gfsk_link_record_in_service_rx(rx_frame.packet.seq);
				}

				k_spin_unlock(&g_link.lock, key);
				prop_gfsk_link_queue_rx_frame(&rx_frame);

				if (config.local_device_role == DEVICE_ROLE_HEADSET) {
					uint32_t measured_tx_tick =	prop_gfsk_link_get_headset_tx_tick(rx_frame.rx_tick, &config);
					uint32_t tx_tick = measured_tx_tick;

					key = k_spin_lock(&g_link.lock);
					bool was_in_service =
						(g_link.state == PROP_GFSK_LINK_STATE_IN_SERVICE);
					g_link.slot_sync.consecutive_missed_peer_rx_frames = 0U;
					prop_gfsk_link_set_in_service_locked();
					k_spin_unlock(&g_link.lock, key);

					(void)prop_gfsk_link_propose_next_tx_tick(
						tx_tick, PROP_GFSK_TX_TICK_SRC_RX_SYNC);

					if (!was_in_service) {
						printk("prop_gfsk: service established\n");
					}
				} else if (config.local_device_role == DEVICE_ROLE_DONGLE) {
					key = k_spin_lock(&g_link.lock);
					bool was_in_service =
						(g_link.state == PROP_GFSK_LINK_STATE_IN_SERVICE);
					g_link.slot_sync.consecutive_missed_peer_rx_frames = 0U;
					prop_gfsk_link_set_in_service_locked();
					k_spin_unlock(&g_link.lock, key);

					if (!was_in_service) {
						printk("prop_gfsk: service established\n");
					}
				}
			}
		}

		/* --- Predicted peer RX window expired without a packet --- */
		if (poll_rc == -EAGAIN) {
			bool lost_service = false;
			k_spinlock_key_t key = k_spin_lock(&g_link.lock);

				if (g_link.state == PROP_GFSK_LINK_STATE_IN_SERVICE) {
					g_link.slot_sync.consecutive_missed_peer_rx_frames++;
					if (g_link.slot_sync.consecutive_missed_peer_rx_frames >=
					    PROP_GFSK_LINK_SYNC_LOSS_FRAMES) {
						prop_gfsk_link_set_no_service_locked();
						lost_service = true;
					}
				}
			k_spin_unlock(&g_link.lock, key);

			if (lost_service) {
				printk("prop_gfsk: service outage (missed predicted rx windows)\n");
			}
		}
	}
}

K_THREAD_DEFINE(prop_gfsk_link_thread_id,
		PROP_GFSK_LINK_THREAD_STACK_SIZE,
		prop_gfsk_link_thread,
		NULL, NULL, NULL,
		PROP_GFSK_LINK_THREAD_PRIORITY, 0, 0);
