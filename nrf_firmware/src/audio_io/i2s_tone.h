#pragma once

#include <stdbool.h>
#include <stdint.h>

bool audio_i2s_tone_is_enabled(void);
void audio_i2s_tone_set_enabled(bool enabled);
uint32_t audio_i2s_tone_get_enqueued_blocks(void);
