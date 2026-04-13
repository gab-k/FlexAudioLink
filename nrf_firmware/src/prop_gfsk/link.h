#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "app_control.h"
#include "prop_gfsk/radio_hw.h"

enum prop_gfsk_link_state {
	PROP_GFSK_LINK_STATE_DISABLED = 0,
	PROP_GFSK_LINK_STATE_NO_SERVICE,
	PROP_GFSK_LINK_STATE_IN_SERVICE,
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
	uint32_t packets_lost_in_service;
	uint32_t loss_burst_count;
	uint32_t loss_burst_1_count;
	uint32_t loss_burst_2_count;
	uint32_t loss_burst_3_4_count;
	uint32_t loss_burst_5_plus_count;
	uint32_t max_loss_burst_len;
	uint32_t crc_error_count;
	uint32_t tx_intended_count;
	uint32_t tx_missed_deadline_count;
	uint32_t pretx_disabled_count;
	uint32_t pretx_rxidle_count;
	uint32_t pretx_rx_count;
	uint32_t pretx_rx_noaddr_count;
	uint32_t pretx_rx_addr_count;
	uint32_t pretx_other_count;
	uint32_t outage_count;
	uint64_t time_in_service_us;
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
