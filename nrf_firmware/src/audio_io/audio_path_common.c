#include "audio_io/audio_path_common.h"

#include <string.h>

#include <zephyr/sys/__assert.h>

const char *audio_path_get_state_name(enum audio_path_state state)
{
	switch (state) {
	case AUDIO_PATH_STATE_PLAYING:
		return "playing";
	case AUDIO_PATH_STATE_BUFFERING:
	default:
		return "buffering";
	}
}

uint32_t audio_ring_push(tu_fifo_t *ff, const uint8_t *data, uint32_t bytes)
{
	uint32_t evicted = 0U;
	uint32_t written = 0U;

	if (ff == NULL || data == NULL || bytes == 0U) {
		return 0U;
	}

	__ASSERT(ff->overwritable, "audio_ring_push requires overwritable FIFO");

	while (written < bytes) {
		uint16_t chunk = (bytes - written) > UINT16_MAX ? UINT16_MAX : (uint16_t)(bytes - written);
		uint32_t before = tu_fifo_count(ff);

		(void)tu_fifo_write_n(ff, data + written, chunk);

		uint32_t after = tu_fifo_count(ff);
		evicted += (before + chunk) - after;
		written += chunk;
	}

	return evicted;
}

uint32_t audio_filter_update(float *filtered, uint32_t level_bytes)
{
	if (filtered == NULL) {
		return 0U;
	}

	if (*filtered < 0.0f) {
		*filtered = (float)level_bytes;
	} else {
		const float alpha = (float)AUDIO_FILTER_ALPHA_NUM / (float)AUDIO_FILTER_ALPHA_DEN;

		*filtered = (alpha * (float)level_bytes) + ((1.0f - alpha) * *filtered);
	}

	return (uint32_t)*filtered;
}

int32_t audio_p_controller_step(int32_t error_bytes, uint32_t level_bytes)
{
	int32_t adjust_hz;

	if (level_bytes <= AUDIO_PANIC_LOW_BYTES) {
		return AUDIO_P_ADJUST_MAX_HZ;
	}
	if (level_bytes >= AUDIO_PANIC_HIGH_BYTES) {
		return -AUDIO_P_ADJUST_MAX_HZ;
	}

	adjust_hz = error_bytes / AUDIO_P_GAIN_DIV;

	if (adjust_hz > AUDIO_P_ADJUST_MAX_HZ) {
		adjust_hz = AUDIO_P_ADJUST_MAX_HZ;
	}
	if (adjust_hz < -AUDIO_P_ADJUST_MAX_HZ) {
		adjust_hz = -AUDIO_P_ADJUST_MAX_HZ;
	}

	return adjust_hz;
}

enum audio_path_state audio_state_advance(enum audio_path_state current, uint32_t level_bytes)
{
	if (current == AUDIO_PATH_STATE_BUFFERING) {
		if (level_bytes >= AUDIO_START_BYTES) {
			return AUDIO_PATH_STATE_PLAYING;
		}
	} else if (level_bytes == 0U) {
		return AUDIO_PATH_STATE_BUFFERING;
	}

	return current;
}

size_t audio_extract_left_to_mono(const uint8_t *stereo, size_t stereo_bytes, uint8_t *mono, size_t mono_max_bytes)
{
	size_t stereo_samples;
	size_t mono_samples;

	if (stereo == NULL || mono == NULL ||
	    stereo_bytes < AUDIO_BYTES_PER_STEREO_SAMPLE ||
	    mono_max_bytes < sizeof(int16_t)) {
		return 0U;
	}

	stereo_samples = stereo_bytes / AUDIO_BYTES_PER_STEREO_SAMPLE;
	mono_samples = mono_max_bytes / sizeof(int16_t);
	if (stereo_samples > mono_samples) {
		stereo_samples = mono_samples;
	}

	for (size_t i = 0; i < stereo_samples; ++i) {
		memcpy(mono + (i * sizeof(int16_t)),
		       stereo + (i * AUDIO_BYTES_PER_STEREO_SAMPLE),
		       sizeof(int16_t));
	}

	return stereo_samples * sizeof(int16_t);
}
