#pragma once

#include <stdint.h>

#include "audio_io/audio_path_common.h"

struct audio_path_wired_status {
	enum audio_path_state stream_state;
	uint32_t spk_level_bytes;            /* combined: tu_fifo_count + i2s pending */
	uint32_t spk_fifo_bytes;             /* tu_fifo_count alone (USB ingress backlog) */
	uint32_t spk_pending_bytes;          /* bytes read out of tu_fifo, not yet clocked out */
	uint32_t spk_filtered_level_bytes;
	int32_t spk_error_bytes;
	int32_t spk_p_adjust_hz;            /* P-controller rate adjustment (Hz) */
	int32_t spk_fll_target_rate_hz;     /* current FLL target sample rate (Hz) */
	uint32_t spk_fll_fails;             /* count of FLL write I2C failures */
	uint32_t spk_underrun_events;       /* count of buffer-drain-to-zero events */
	uint32_t mic_level_bytes;
	uint32_t mic_overflow_bytes;
};

void audio_path_wired_init(void);
void audio_path_wired_get_status(struct audio_path_wired_status *out);

void audio_path_wired_fll_set_fixed(int32_t rate_hz);
void audio_path_wired_fll_set_auto(void);
int32_t audio_path_wired_fll_get_fixed_rate(void);
