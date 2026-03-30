#pragma once

enum audio_io_id {
	AUDIO_IO_NONE = 0,
	AUDIO_IO_USB_PLAYBACK,
	AUDIO_IO_USB_CAPTURE,
	AUDIO_IO_I2S_PLAYBACK,
	AUDIO_IO_I2S_CAPTURE,
	AUDIO_IO_TONE_GENERATOR,
};

const char *audio_io_get_name(enum audio_io_id io);
