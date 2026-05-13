#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/sys/util.h>
#include "prop/radio_core.h"

#define BYTES_PER_STEREO_SAMPLE      4U
/* Audio bytes stored in prop_packet.payload. prop_packet.length adds PROP metadata. */
#define PROP_SPK_PACKET_BYTES        192U
#define PROP_MIC_PACKET_BYTES        96U

BUILD_ASSERT(PROP_SPK_PACKET_BYTES <= PROP_PAYLOAD_MAX_LEN,
	     "speaker PROP packet must fit payload");
BUILD_ASSERT(PROP_MIC_PACKET_BYTES <= PROP_PAYLOAD_MAX_LEN,
	     "mic PROP packet must fit payload");
BUILD_ASSERT((PROP_PACKET_METADATA_LEN + PROP_SPK_PACKET_BYTES) <=
	     PROP_PACKET_MAX_LEN,
	     "speaker PROP frame length must fit packet");
BUILD_ASSERT((PROP_PACKET_METADATA_LEN + PROP_MIC_PACKET_BYTES) <=
	     PROP_PACKET_MAX_LEN,
	     "mic PROP frame length must fit packet");

#define FILTER_ALPHA_NUM             1
#define FILTER_ALPHA_DEN             5
#define P_GAIN                       (1.25f)
#define P_TERM_MAX_HZ                400
#define P_ADJUST_MAX_HZ              800
#define P_KI                         (0.005f)
#define I_MAX_HZ                     400

#define CTRL_DEBUG_LOG
#define WARN_SPK_LVL
#define WARN_COOLDOWN_MS	2000

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

const char *path_get_state_name(enum path_state state);

void update_codec_clock(uint32_t target,
			uint32_t fifo, uint32_t pending,
			uint32_t *filtered_level_bytes,
			int32_t *error_bytes,
			int32_t *p_adjust_hz,
			int32_t *fll_target_rate_hz);

void warn_on_level(uint32_t level, uint32_t fifo_bytes, uint32_t pending_bytes, uint32_t warn_low, uint32_t warn_high);

extern struct fll_state fll;

bool fll_set_fixed(int32_t rate_hz);
void fll_set_auto(void);
int32_t fll_get_fixed_rate(void);
