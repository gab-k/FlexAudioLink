#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "audio_io/audio_path_common.h"

struct audio_path_wired_status {
	bool active;
	enum audio_path_state stream_state;
	uint32_t spk_level_bytes;            /* combined: tu_fifo_count + i2s pending */
	uint32_t spk_fifo_bytes;             /* tu_fifo_count alone (USB ingress backlog) */
	uint32_t spk_pending_bytes;          /* bytes read out of tu_fifo, not yet clocked out */
	uint32_t spk_filtered_level_bytes;
	int32_t spk_error_bytes;
	int32_t spk_p_adjust_hz;
	uint32_t mic_level_bytes;
	uint32_t mic_overflow_bytes;
};

void audio_path_wired_activate(void);
void audio_path_wired_deactivate(void);
void audio_path_wired_get_status(struct audio_path_wired_status *out);
