#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <zephyr/kernel.h>

#include "app_control.h"
#include "prop_gfsk/radio_hw.h"

enum pgfsk_link_state {
	PGFSK_LINK_STATE_DISABLED = 0,
	PGFSK_LINK_STATE_NO_SERVICE,
	PGFSK_LINK_STATE_IN_SERVICE,
};

struct pgfsk_link_config {
	bool enabled;
	enum device_role local_device_role;
};

struct pgfsk_frame {
	uint16_t seq;
	uint8_t payload[PGFSK_PAYLOAD_LEN];
	size_t len;
	int16_t rssi_dbm;
};

struct pgfsk_link_report {
	uint32_t packets_tx;
	uint32_t packets_rx;
	uint32_t packets_lost_in_service;
	uint32_t loss_burst_1_count;
	uint32_t loss_burst_2_count;
	uint32_t loss_burst_3_4_count;
	uint32_t loss_burst_5_plus_count;
	uint32_t max_loss_burst_len;
	uint32_t crc_error_count;
	uint32_t tx_intended_count;
	uint32_t tx_missed_deadline_count;
	uint32_t outage_count;
	uint64_t time_in_service_us;
	uint32_t last_prepare_us;
	uint32_t max_prepare_us;
	uint32_t last_turnaround_us;
	uint32_t max_turnaround_us;
	int16_t last_rssi_dbm;
	enum pgfsk_link_state state;
};

bool pgfsk_link_set_config(const struct pgfsk_link_config *config);
bool pgfsk_link_stop(void);
bool pgfsk_link_is_enabled(void);
enum pgfsk_link_state pgfsk_link_get_state(void);
void pgfsk_link_get_report(struct pgfsk_link_report *report);
bool pgfsk_link_tx_enqueue(const struct pgfsk_frame *frame, k_timeout_t timeout);
bool pgfsk_link_rx_dequeue(struct pgfsk_frame *frame, k_timeout_t timeout);
