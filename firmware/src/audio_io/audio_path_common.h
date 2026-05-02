#pragma once

#include <stdint.h>

#include <zephyr/sys/util.h>

#include "tusb.h"

#include "prop_fsk/radio_core.h"

#define AUDIO_BYTES_PER_STEREO_SAMPLE      4U
/* Audio bytes stored in pfsk_packet.payload. pfsk_packet.length adds PFSK metadata. */
#define AUDIO_PFSK_SPK_PACKET_BYTES        192U
#define AUDIO_PFSK_MIC_PACKET_BYTES        96U

BUILD_ASSERT(AUDIO_PFSK_SPK_PACKET_BYTES <= PFSK_PAYLOAD_MAX_LEN,
	     "speaker PFSK packet must fit payload");
BUILD_ASSERT(AUDIO_PFSK_MIC_PACKET_BYTES <= PFSK_PAYLOAD_MAX_LEN,
	     "mic PFSK packet must fit payload");
BUILD_ASSERT((PFSK_PACKET_METADATA_LEN + AUDIO_PFSK_SPK_PACKET_BYTES) <=
	     PFSK_PACKET_MAX_LEN,
	     "speaker PFSK frame length must fit packet");
BUILD_ASSERT((PFSK_PACKET_METADATA_LEN + AUDIO_PFSK_MIC_PACKET_BYTES) <=
	     PFSK_PACKET_MAX_LEN,
	     "mic PFSK frame length must fit packet");

/* Start threshold kept equal to target so BUFFERING->PLAYING fires exactly at
 * the nominal operating level. Both defines retained for future tuning. */
#define AUDIO_START_BYTES                  480U
#define AUDIO_TARGET_BYTES                 480U
#define AUDIO_PANIC_LOW_BYTES              192U

#define AUDIO_FILTER_ALPHA_NUM             1
#define AUDIO_FILTER_ALPHA_DEN             5
#define AUDIO_P_GAIN                       (1.25f)
#define AUDIO_P_TERM_MAX_HZ                400
#define AUDIO_P_ADJUST_MAX_HZ              800
#define AUDIO_P_KI                         (0.005f)
#define AUDIO_I_MAX_HZ                     400

/* Uncomment to enable controller debug log every ~1 second. */
#define AUDIO_CTRL_DEBUG_LOG

/* Shared FLL update throttling parameters. */
#define AUDIO_FLL_UPDATE_INTERVAL_MS       100

enum audio_path_state {
	AUDIO_PATH_STATE_BUFFERING = 0,
	AUDIO_PATH_STATE_PLAYING,
};

const char *audio_path_get_state_name(enum audio_path_state state);

enum audio_path_state audio_state_advance(enum audio_path_state current, uint32_t level_bytes);

/*
 * Shared adaptive codec clock PI controller.
 * Runs EMA filter + proportional + integral control on the
 * given buffer level, returning the recommended FLL adjustment in Hz.
 *   target      desired buffer level in bytes
 *   filter      EMA state pointer (set to -1.0f on reset)
 *   i_sum       integral accumulator pointer (set to 0.0f on reset)
 *   gain_mult   proportional gain multiplier (error_bytes * gain_mult → Hz)
 *   ki          integral gain (fraction of error added per update)
 *   fifo        USB FIFO fill in bytes
 *   pending     I2S pipeline bytes in flight
 *
 *   level = fifo + pending is computed internally.
 */
int32_t audio_codec_clock_controller(uint32_t target,
				     float *filter, float *i_sum,
				     float gain_mult, float ki,
				     uint32_t fifo, uint32_t pending);
