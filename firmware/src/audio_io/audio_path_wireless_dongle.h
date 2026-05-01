#pragma once

#include "audio_io/audio_path_wireless.h"

void audio_path_wireless_dongle_init(void);
void audio_path_wireless_dongle_get_status(struct audio_path_wireless_status *out);
