#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "app_control.h"
#include "prop_gfsk/radio_hw.h"

enum prop_gfsk_link_state {
	PROP_GFSK_LINK_STATE_DISABLED = 0,
	PROP_GFSK_LINK_STATE_SEARCHING,
	PROP_GFSK_LINK_STATE_RUNNING,
};

struct prop_gfsk_link_config {
	bool enabled;
	enum device_role local_device_role;
	uint32_t dongle_tx_slot_us;
	uint32_t headset_tx_slot_us;
};

struct prop_gfsk_frame {
	uint16_t seq;
	uint8_t payload[PROP_GFSK_PAYLOAD_LEN];
	size_t len;
	int16_t rssi_dbm;
};

struct prop_gfsk_link_report {
	uint32_t packets_tx;
	uint32_t packets_rx;
	uint32_t packets_lost_total;
	uint32_t packets_lost_while_locked;
	uint32_t lock_acquire_count;
	uint32_t lock_loss_count;
	uint64_t time_locked_us;
	int16_t last_rssi_dbm;
	enum prop_gfsk_link_state state;
};

bool prop_gfsk_link_set_config(const struct prop_gfsk_link_config *config);
bool prop_gfsk_link_stop(void);
bool prop_gfsk_link_is_enabled(void);
enum prop_gfsk_link_state prop_gfsk_link_get_state(void);
void prop_gfsk_link_get_report(struct prop_gfsk_link_report *report);
bool prop_gfsk_link_tx_enqueue(const struct prop_gfsk_frame *frame,
			       k_timeout_t timeout);
bool prop_gfsk_link_rx_dequeue(struct prop_gfsk_frame *frame,
			       k_timeout_t timeout);
