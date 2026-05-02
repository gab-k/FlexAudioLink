#pragma once

#include <stdbool.h>

#include <zephyr/kernel.h>

#include "prop_fsk/radio_core.h"

enum pfsk_session_state {
	PFSK_SESSION_STATE_NO_SERVICE = 0,
	PFSK_SESSION_STATE_IN_SERVICE,
};

struct pfsk_session_report {
	uint32_t packets_tx;
	uint32_t rx_ok_count;
	uint32_t packets_lost_in_service;
	uint32_t loss_burst_1_count;
	uint32_t loss_burst_2_count;
	uint32_t loss_burst_3_4_count;
	uint32_t loss_burst_5_plus_count;
	uint32_t max_loss_burst_len;
	uint32_t crc_error_count;
	uint32_t deadline_late_count;
	uint32_t rx_incomplete_count;
	uint32_t tx_trigger_fail_count;
	uint32_t outage_count;
	uint64_t time_in_service_us;
	int16_t last_rssi_dbm;
	enum pfsk_session_state state;
};

bool pfsk_session_start_dongle(void);
bool pfsk_session_start_headset(void);
enum pfsk_session_state pfsk_session_get_state(void);
void pfsk_session_get_report(struct pfsk_session_report *report);
bool pfsk_session_tx_enqueue(const struct pfsk_packet *packet, k_timeout_t timeout);
bool pfsk_session_rx_dequeue(struct pfsk_packet *packet, k_timeout_t timeout);
