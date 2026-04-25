#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "audio_io/audio_path_common.h"

struct audio_path_wireless_status {
	bool active;
	enum audio_path_state stream_state;
	uint32_t spk_level_bytes;
	uint32_t spk_underrun_bytes;
	uint32_t overflow_bytes;
	uint32_t spk_silence_inserted_bytes;
	uint32_t spk_dropped_oldest_bytes;
	uint32_t spk_usb_level_bytes;
	uint32_t mic_usb_level_bytes;
	int32_t peer_adjust_hz;
	int32_t spk_p_adjust_hz;
	uint32_t rx_malformed_frames;
};

void audio_path_wireless_activate(void);
void audio_path_wireless_deactivate(void);
void audio_path_wireless_get_status(struct audio_path_wireless_status *out);
