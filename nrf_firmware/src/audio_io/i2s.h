#pragma once

#include <stdbool.h>

bool audio_i2s_is_ready(void);
bool audio_i2s_is_tone_enabled(void);
void audio_i2s_set_tone_enabled(bool enabled);
