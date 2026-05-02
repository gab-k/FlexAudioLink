#pragma once

#include <stdint.h>

#include "audio_io/audio_path_common.h"

struct audio_path_wireless_headset_status {
	enum audio_path_state stream_state;
	uint32_t spk_fifo_bytes;
	uint32_t spk_pending_bytes;
	uint32_t spk_filtered_level_bytes;
	int32_t spk_error_bytes;
	int32_t spk_p_adjust_hz;
	int32_t spk_fll_target_rate_hz;
	uint32_t spk_underrun_events;
};

void audio_path_wireless_headset_init(void);
void audio_path_wireless_headset_get_status(struct audio_path_wireless_headset_status *out);

void audio_path_wireless_headset_fll_set_fixed(int32_t rate_hz);
void audio_path_wireless_headset_fll_set_auto(void);
int32_t audio_path_wireless_headset_fll_get_fixed_rate(void);
