#pragma once

#include "audio_io/audio_path_wireless.h"

void audio_path_wireless_headset_init(void);
void audio_path_wireless_headset_get_status(struct audio_path_wireless_status *out);

void audio_path_wireless_headset_fll_set_fixed(int32_t rate_hz);
void audio_path_wireless_headset_fll_set_auto(void);
int32_t audio_path_wireless_headset_fll_get_fixed_rate(void);
