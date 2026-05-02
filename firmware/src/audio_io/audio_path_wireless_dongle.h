#pragma once

#include <stdint.h>

#include "audio_io/audio_path_common.h"

struct audio_path_wireless_dongle_status {
	uint32_t overflow_bytes;
};

void audio_path_wireless_dongle_init(void);
void audio_path_wireless_dongle_get_status(struct audio_path_wireless_dongle_status *out);
