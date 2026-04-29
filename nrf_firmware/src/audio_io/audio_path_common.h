#pragma once

#include <stddef.h>
#include <stdint.h>

#include "tusb.h"

#define AUDIO_BYTES_PER_STEREO_SAMPLE      4U
#define AUDIO_STEP_BYTES                   192U

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

uint32_t audio_filter_update(float *filtered, uint32_t level_bytes);
int32_t audio_p_controller_step(int32_t error_bytes, uint32_t level_bytes);
enum audio_path_state audio_state_advance(enum audio_path_state current, uint32_t level_bytes);
size_t audio_extract_left_to_mono(const uint8_t *stereo, size_t stereo_bytes, uint8_t *mono, size_t mono_max_bytes);

/*
 * Shared adaptive codec clock PI controller.
 * Runs EMA filter + proportional + integral control on the
 * given buffer level, returning the recommended FLL adjustment in Hz.
 *   level       current combined buffer-fill (fifo + pipeline)
 *   target      desired buffer level in bytes
 *   filter      EMA state pointer (set to -1.0f on reset)
 *   i_sum       integral accumulator pointer (set to 0.0f on reset)
 *   gain_mult   proportional gain multiplier (error_bytes * gain_mult → Hz)
 *   ki          integral gain (fraction of error added per update)
 */
int32_t audio_codec_clock_controller(uint32_t level, uint32_t target,
				     float *filter, float *i_sum,
				     float gain_mult, float ki);
