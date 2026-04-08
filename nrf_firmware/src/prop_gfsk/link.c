#include "prop_gfsk/link.h"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define PROP_GFSK_LINK_THREAD_STACK_SIZE   2048
#define PROP_GFSK_LINK_THREAD_PRIORITY     6
#define PROP_GFSK_LINK_QUEUE_DEPTH         4
#define PROP_GFSK_LINK_DEFAULT_DONGLE_TX_SLOT_US  1000U
#define PROP_GFSK_LINK_DEFAULT_HEADSET_TX_SLOT_US 1000U
/*
 * Lock-loss timeout: if no packet arrives within this many frame intervals,
 * the headset declares lock loss.  Replaces the old miss-count mechanism.
 */
#define PROP_GFSK_LINK_SYNC_LOSS_FRAMES    8U
#define PROP_GFSK_LINK_CONFIG_SET_TIMEOUT  K_MSEC(1000)

struct link_stats {
	uint32_t packets_lost_while_locked;
	uint32_t lock_acquire_count;
	uint32_t lock_loss_count;
	uint64_t time_locked_us;
};

struct prop_gfsk_link_runtime {
	struct prop_gfsk_link_config config;
	enum prop_gfsk_link_state state;
	struct link_stats stats;
	uint16_t tx_seq;
	struct {
		uint16_t last_rx_seq;
		bool have_last_rx_seq;
		uint64_t locked_since_cyc;
		uint32_t next_expected_dongle_rx_tick;
		uint8_t consecutive_missed_rx_frames;
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

static void prop_gfsk_link_note_locked_time(uint64_t now_cyc)
{
	if (g_link.config.local_device_role != DEVICE_ROLE_HEADSET ||
	    g_link.state != PROP_GFSK_LINK_STATE_RUNNING ||
	    g_link.slot_sync.locked_since_cyc == 0U) {
		return;
	}

	if (now_cyc > g_link.slot_sync.locked_since_cyc) {
		g_link.stats.time_locked_us +=
			k_cyc_to_us_floor64(now_cyc -
					    g_link.slot_sync.locked_since_cyc);
	}

	g_link.slot_sync.locked_since_cyc = 0U;
}

static void prop_gfsk_link_set_searching_locked(void)
{
	prop_gfsk_link_note_locked_time(k_cycle_get_64());
	if (g_link.config.local_device_role == DEVICE_ROLE_HEADSET &&
	    g_link.state == PROP_GFSK_LINK_STATE_RUNNING) {
		g_link.stats.lock_loss_count++;
	}
	g_link.state = g_link.config.enabled ? PROP_GFSK_LINK_STATE_SEARCHING
					     : PROP_GFSK_LINK_STATE_DISABLED;
	g_link.slot_sync.have_last_rx_seq = false;
	g_link.slot_sync.consecutive_missed_rx_frames = 0U;
}

static void prop_gfsk_link_set_running_locked(void)
{
	if (g_link.config.local_device_role == DEVICE_ROLE_HEADSET &&
	    g_link.state != PROP_GFSK_LINK_STATE_RUNNING) {
		g_link.stats.lock_acquire_count++;
		g_link.slot_sync.locked_since_cyc = k_cycle_get_64();
	}
	g_link.state = PROP_GFSK_LINK_STATE_RUNNING;
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

static k_timeout_t prop_gfsk_link_get_headset_poll_timeout(uint32_t frame_interval_us,
							   uint32_t dongle_tx_slot_us)
{
	uint32_t now_tick;
	uint32_t deadline_tick;
	int32_t delta_us;

	if (g_link.state != PROP_GFSK_LINK_STATE_RUNNING) {
		return K_FOREVER;
	}

	now_tick = prop_gfsk_radio_hw_get_tick();
	deadline_tick = g_link.slot_sync.next_expected_dongle_rx_tick;
	if (dongle_tx_slot_us > PROP_GFSK_PRE_TX_OFFSET_US) {
		deadline_tick += dongle_tx_slot_us - PROP_GFSK_PRE_TX_OFFSET_US;
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

static void prop_gfsk_link_record_locked_rx(uint16_t seq)
{
	if (g_link.slot_sync.have_last_rx_seq) {
		uint16_t expected = g_link.slot_sync.last_rx_seq + 1U;
		uint16_t gap;

		if (prop_gfsk_link_seq_gap_from_expected(expected, seq, &gap) &&
		    gap > 0U) {
			g_link.stats.packets_lost_while_locked += gap;
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

static void prop_gfsk_link_prepare_tx_packet(struct prop_gfsk_packet *packet)
{
	struct prop_gfsk_frame frame;
	bool has_payload = (k_msgq_get(&g_prop_gfsk_tx_queue, &frame, K_NO_WAIT) == 0);

	memset(packet, 0, sizeof(*packet));
	packet->length = PROP_GFSK_PACKET_METADATA_LEN;
	packet->seq = g_link.tx_seq++;

	if (!has_payload) {
		packet->payload_len = 0U;
		return;
	}

	packet->payload_len = frame.len;
	packet->length = PROP_GFSK_PACKET_METADATA_LEN + packet->payload_len;
	memcpy(packet->data, frame.payload, frame.len);
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
	printk("Sync word: 0x%08lX\n\n", PROP_GFSK_RADIO_SYNC_WORD);
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
	g_link.slot_sync.locked_since_cyc = 0U;
	g_link.slot_sync.next_expected_dongle_rx_tick = 0U;
	g_link.slot_sync.consecutive_missed_rx_frames = 0U;
	memset(&g_link.stats, 0, sizeof(g_link.stats));

	if (!config->enabled) {
		g_link.state = PROP_GFSK_LINK_STATE_DISABLED;
	} else if (config->local_device_role == DEVICE_ROLE_DONGLE) {
		prop_gfsk_link_set_running_locked();
	} else {
		prop_gfsk_link_set_searching_locked();
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

	if (g_link.config.local_device_role == DEVICE_ROLE_HEADSET &&
	    state == PROP_GFSK_LINK_STATE_RUNNING &&
	    g_link.slot_sync.locked_since_cyc != 0U) {
		stats.time_locked_us += k_cyc_to_us_floor64(
			k_cycle_get_64() - g_link.slot_sync.locked_since_cyc);
	}

	k_spin_unlock(&g_link.lock, key);

	*report = (struct prop_gfsk_link_report){
		.packets_tx                = hw_stats.packets_tx,
		.packets_rx                = hw_stats.packets_rx,
		.packets_lost_total        = hw_stats.packets_lost,
		.last_rssi_dbm             = hw_stats.last_rssi_dbm,
		.packets_lost_while_locked = stats.packets_lost_while_locked,
		.lock_acquire_count        = stats.lock_acquire_count,
		.lock_loss_count           = stats.lock_loss_count,
		.time_locked_us            = stats.time_locked_us,
		.state                     = state,
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

	uint32_t next_dongle_tx_tick = 0U;

	while (1) {
		/* Check for pending config changes before anything else. */
		if (prop_gfsk_link_process_config_request(K_NO_WAIT)) {
			next_dongle_tx_tick = 0U;
			continue;
		}

		struct prop_gfsk_link_config config = prop_gfsk_link_get_config();

		if (!config.enabled) {
			(void)prop_gfsk_link_process_config_request(K_MSEC(20));
			continue;
		}

		/*
		 * Dongle: seed the TX schedule on (re)start.
		 */
		if (config.local_device_role == DEVICE_ROLE_DONGLE &&
		    next_dongle_tx_tick == 0U) {
			struct prop_gfsk_packet tx_packet;

			next_dongle_tx_tick = prop_gfsk_radio_hw_get_tick() + config.dongle_tx_slot_us +
					       config.headset_tx_slot_us;
			prop_gfsk_link_prepare_tx_packet(&tx_packet);
			prop_gfsk_radio_hw_schedule_tx(next_dongle_tx_tick, &tx_packet);
		}

		uint32_t frame_interval_us = config.dongle_tx_slot_us + config.headset_tx_slot_us;
		k_timeout_t rx_timeout = (config.local_device_role == DEVICE_ROLE_HEADSET) ?
						 prop_gfsk_link_get_headset_poll_timeout(
							 frame_interval_us, config.dongle_tx_slot_us) :
						 K_FOREVER;

		/* Reset poll states and wait. */
		events[0].state = K_POLL_STATE_NOT_READY;
		events[1].state = K_POLL_STATE_NOT_READY;
		int poll_rc = k_poll(events, ARRAY_SIZE(events), rx_timeout);

		/* --- TX done (primarily for the dongle) --- */
		if (events[1].state == K_POLL_STATE_SEM_AVAILABLE) {
			k_sem_take(prop_gfsk_radio_hw_tx_done_sem(), K_NO_WAIT);

			if (config.local_device_role == DEVICE_ROLE_DONGLE) {
				struct prop_gfsk_packet tx_packet;

				next_dongle_tx_tick += frame_interval_us;
				prop_gfsk_link_prepare_tx_packet(&tx_packet);
				prop_gfsk_radio_hw_schedule_tx(next_dongle_tx_tick, &tx_packet);
			} else if (config.local_device_role == DEVICE_ROLE_HEADSET) {
				uint32_t tx_tick = 0U;
				bool schedule_tx = false;
				k_spinlock_key_t key = k_spin_lock(&g_link.lock);

				if (g_link.state == PROP_GFSK_LINK_STATE_RUNNING) {
					tx_tick = g_link.slot_sync.next_expected_dongle_rx_tick +
						  config.dongle_tx_slot_us;
					schedule_tx = true;
				}

				k_spin_unlock(&g_link.lock, key);

				if (schedule_tx) {
					struct prop_gfsk_packet tx_packet;

					prop_gfsk_link_prepare_tx_packet(&tx_packet);
					prop_gfsk_radio_hw_schedule_tx(tx_tick, &tx_packet);
				}
			}
		}

		/* --- RX packet --- */
		if (events[0].state == K_POLL_STATE_MSGQ_DATA_AVAILABLE) {
			struct prop_gfsk_rx_frame rx_frame;

			while (prop_gfsk_radio_hw_rx_dequeue(&rx_frame, K_NO_WAIT)) {
				k_spinlock_key_t key = k_spin_lock(&g_link.lock);

				if (config.local_device_role == DEVICE_ROLE_HEADSET &&
				    g_link.state == PROP_GFSK_LINK_STATE_RUNNING) {
					prop_gfsk_link_record_locked_rx(rx_frame.packet.seq);
				}

				k_spin_unlock(&g_link.lock, key);
				prop_gfsk_link_queue_rx_frame(&rx_frame);

				if (config.local_device_role == DEVICE_ROLE_HEADSET) {
					uint32_t tx_tick = rx_frame.rx_tick + config.dongle_tx_slot_us;
					struct prop_gfsk_packet tx_packet;

					key = k_spin_lock(&g_link.lock);
					bool was_running = (g_link.state == PROP_GFSK_LINK_STATE_RUNNING);
					g_link.slot_sync.next_expected_dongle_rx_tick =
						rx_frame.rx_tick + frame_interval_us;
					g_link.slot_sync.consecutive_missed_rx_frames = 0U;
					prop_gfsk_link_set_running_locked();
					k_spin_unlock(&g_link.lock, key);

					prop_gfsk_link_prepare_tx_packet(&tx_packet);
					prop_gfsk_radio_hw_schedule_tx(tx_tick, &tx_packet);

					if (!was_running) {
						printk("prop_gfsk: lock acquired\n");
					}
				}
			}
		}

		/* --- Predicted headset RX window expired without a packet --- */
		if (config.local_device_role == DEVICE_ROLE_HEADSET && poll_rc == -EAGAIN) {
			bool lost_lock = false;
			k_spinlock_key_t key = k_spin_lock(&g_link.lock);

			if (g_link.state == PROP_GFSK_LINK_STATE_RUNNING) {
				g_link.slot_sync.consecutive_missed_rx_frames++;
				if (g_link.slot_sync.consecutive_missed_rx_frames >=
				    PROP_GFSK_LINK_SYNC_LOSS_FRAMES) {
					prop_gfsk_link_set_searching_locked();
					lost_lock = true;
				} else {
					g_link.slot_sync.next_expected_dongle_rx_tick += frame_interval_us;
				}
			}
			k_spin_unlock(&g_link.lock, key);

			if (lost_lock) {
				printk("prop_gfsk: lock lost (missed predicted rx windows)\n");
			}
		}
	}
}

K_THREAD_DEFINE(prop_gfsk_link_thread_id,
		PROP_GFSK_LINK_THREAD_STACK_SIZE,
		prop_gfsk_link_thread,
		NULL, NULL, NULL,
		PROP_GFSK_LINK_THREAD_PRIORITY, 0, 0);
