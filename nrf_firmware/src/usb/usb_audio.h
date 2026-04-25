#pragma once

#include <stddef.h>
#include <stdint.h>

void usb_audio_reset(void);

size_t usb_audio_write_microphone_bytes(const uint8_t *data, size_t bytes);

uint32_t usb_audio_microphone_level_bytes(void);
