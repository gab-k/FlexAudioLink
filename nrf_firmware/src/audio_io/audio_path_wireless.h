#pragma once

#include <stdint.h>

#include "audio_io/audio_path_common.h"

struct audio_path_wireless_status {
	enum audio_path_state stream_state;
	uint32_t spk_level_bytes;
	uint32_t spk_filtered_level_bytes;
	uint32_t spk_underrun_bytes;
	uint32_t overflow_bytes;
	uint32_t spk_silence_inserted_bytes;
	uint32_t spk_dropped_oldest_bytes;
	uint32_t spk_usb_level_bytes;
	uint32_t mic_usb_level_bytes;
	int32_t spk_p_adjust_hz;
	int32_t spk_fll_target_rate_hz;
	uint32_t spk_fll_fails;
};
