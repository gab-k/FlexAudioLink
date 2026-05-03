#pragma once

#include <stdint.h>

#include "audio/path_common.h"

struct path_headset_status {
	enum path_state stream_state;
	uint32_t spk_fifo_bytes;
	uint32_t spk_pending_bytes;
	uint32_t spk_filtered_level_bytes;
	int32_t spk_error_bytes;
	int32_t spk_p_adjust_hz;
	int32_t spk_fll_target_rate_hz;
	uint32_t spk_underrun_events;
};

void path_headset_init(void);
void path_headset_get_status(struct path_headset_status *out);
