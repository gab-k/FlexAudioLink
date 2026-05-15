#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/sys/util.h>
#include "prop/radio_core.h"

#define BYTES_PER_STEREO_SAMPLE      4U
/* Audio bytes stored in prop_packet.data. prop_packet.length adds PROP metadata. */
#define PROP_SPK_PACKET_BYTES        192U
#define PROP_MIC_PACKET_BYTES        96U

BUILD_ASSERT(PROP_SPK_PACKET_BYTES <= PROP_DATA_MAX_LEN,
	     "speaker data doesnt fit prop packet data section");
BUILD_ASSERT(PROP_MIC_PACKET_BYTES <= PROP_DATA_MAX_LEN,
	     "mic data doesnt fit prop packet data section");
BUILD_ASSERT((PROP_PACKET_METADATA_LEN + PROP_SPK_PACKET_BYTES) <=
	     PROP_PACKET_MAX_LEN,
	     "speaker frame length doesnt fit packet max length");
BUILD_ASSERT((PROP_PACKET_METADATA_LEN + PROP_MIC_PACKET_BYTES) <=
	     PROP_PACKET_MAX_LEN,
	     "mic frame length doesnt fit packet max length");

#define FILTER_ALPHA_NUM             1
#define FILTER_ALPHA_DEN             5
#define P_GAIN                       (1.25f)
#define P_TERM_MAX_HZ                400
#define FLL_ADJUST_MAX_HZ            800
#define P_KI                         (0.005f)
#define I_MAX_HZ                     400

#define CTRL_DEBUG_LOG
#define CTRL_DEBUG_LOG_INTERVAL_MS	1000
#define WARN_SPK_LVL
#define WARN_COOLDOWN_MS	2000

#define EMA_FILTER_UPDATE_INTERVAL_MS       10
/* Shared FLL update throttling parameters. */
#define FLL_UPDATE_INTERVAL_MS       100

enum path_state {
	PATH_STATE_BUFFERING = 0,
	PATH_STATE_PLAYING,
};

struct fll_state {
	bool fixed;
	int32_t fixed_rate_hz;
};

struct codec_path_status {
	enum path_state stream_state;
	uint32_t spk_fifo_bytes;
	uint32_t spk_pending_bytes;
	uint32_t spk_filtered_level_bytes;
	int32_t spk_error_bytes;
	int32_t spk_p_adjust_hz;
	int32_t spk_fll_target_rate_hz;
	uint32_t spk_underrun_events;
};

const char *path_get_state_name(enum path_state state);

uint32_t codec_level_filter_update(float *filtered, uint32_t level_bytes);
int32_t codec_clock_controller(int32_t error_bytes, uint32_t nominal_rate_hz, int32_t *out_fll_target_rate_hz);

void monitor_codec_level(const struct codec_path_status *status, uint32_t warn_low, uint32_t warn_high);

extern struct fll_state fll;

bool fll_set_fixed(int32_t rate_hz);
void fll_set_auto(void);
int32_t fll_get_fixed_rate(void);
