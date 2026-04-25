#pragma once

#include <stddef.h>
#include <stdint.h>

#include "tusb.h"

#define AUDIO_BYTES_PER_STEREO_SAMPLE      4U
#define AUDIO_BYTES_PER_MS                 192U
#define AUDIO_STEP_BYTES                   96U
#define AUDIO_DMA_MAX_BYTES                384U

/* Start threshold kept equal to target so BUFFERING->PLAYING fires exactly at
 * the nominal operating level. Both defines retained for future tuning. */
#define AUDIO_START_BYTES                  480U
#define AUDIO_TARGET_BYTES                 480U
#define AUDIO_PANIC_LOW_BYTES              192U
#define AUDIO_PANIC_HIGH_BYTES             864U

#define AUDIO_FILTER_ALPHA_NUM             1
#define AUDIO_FILTER_ALPHA_DEN             20
#define AUDIO_P_GAIN_DIV                   8
#define AUDIO_P_ADJUST_MAX_HZ              100

enum audio_path_state {
	AUDIO_PATH_STATE_BUFFERING = 0,
	AUDIO_PATH_STATE_PLAYING,
};

const char *audio_path_get_state_name(enum audio_path_state state);

uint32_t audio_ring_push(tu_fifo_t *ff, const uint8_t *data, uint32_t bytes);
uint32_t audio_filter_update(float *filtered, uint32_t level_bytes);
int32_t audio_p_controller_step(int32_t error_bytes, uint32_t level_bytes);
enum audio_path_state audio_state_advance(enum audio_path_state current, uint32_t level_bytes);
size_t audio_extract_left_to_mono(const uint8_t *stereo, size_t stereo_bytes, uint8_t *mono, size_t mono_max_bytes);
