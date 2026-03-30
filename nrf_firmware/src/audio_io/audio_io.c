#include "audio_io/audio_io.h"

const char *audio_io_get_name(enum audio_io_id io)
{
	switch (io) {
	case AUDIO_IO_USB_PLAYBACK:
		return "usb-playback";
	case AUDIO_IO_USB_CAPTURE:
		return "usb-capture";
	case AUDIO_IO_I2S_PLAYBACK:
		return "i2s-playback";
	case AUDIO_IO_I2S_CAPTURE:
		return "i2s-capture";
	case AUDIO_IO_TONE_GENERATOR:
		return "tone-generator";
	case AUDIO_IO_NONE:
	default:
		return "none";
	}
}
