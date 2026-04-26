#include "audio_io/audio_path_common.h"

#include <string.h>

#include <zephyr/sys/printk.h>

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
	(void)level_bytes;

	if (error_bytes > AUDIO_P_ADJUST_MAX_HZ) {
		return AUDIO_P_ADJUST_MAX_HZ;
	}
	if (error_bytes < -AUDIO_P_ADJUST_MAX_HZ) {
		return -AUDIO_P_ADJUST_MAX_HZ;
	}

	return error_bytes;
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

int32_t audio_codec_clock_controller(uint32_t level, uint32_t target,
				     float *filter, float *i_sum,
				     float gain_mult, float ki)
{
	uint32_t filtered;
	int32_t error_bytes;
	int32_t p_out;
	int32_t output;

	if (filter == NULL || i_sum == NULL) {
		return 0;
	}

	filtered = audio_filter_update(filter, level);
	error_bytes = (int32_t)target - (int32_t)filtered;

	p_out = (int32_t)((float)audio_p_controller_step(error_bytes, filtered) * gain_mult);
	if (p_out > AUDIO_P_TERM_MAX_HZ) {
		p_out = AUDIO_P_TERM_MAX_HZ;
	} else if (p_out < -AUDIO_P_TERM_MAX_HZ) {
		p_out = -AUDIO_P_TERM_MAX_HZ;
	}

	*i_sum += (float)error_bytes * ki;

	if (*i_sum > (float)AUDIO_I_MAX_HZ) {
		*i_sum = (float)AUDIO_I_MAX_HZ;
	} else if (*i_sum < -(float)AUDIO_I_MAX_HZ) {
		*i_sum = -(float)AUDIO_I_MAX_HZ;
	}

	output = p_out + (int32_t)*i_sum;

	if (output > AUDIO_P_ADJUST_MAX_HZ) {
		output = AUDIO_P_ADJUST_MAX_HZ;
		if (error_bytes < 0) {
			*i_sum = (float)output - (float)p_out;
		}
	} else if (output < -AUDIO_P_ADJUST_MAX_HZ) {
		output = -AUDIO_P_ADJUST_MAX_HZ;
		if (error_bytes > 0) {
			*i_sum = (float)output - (float)p_out;
		}
	}

	{
		static uint32_t log_cnt = 0;

		#ifdef AUDIO_CTRL_DEBUG_LOG 
		if (++log_cnt % 10 == 0) {
			int32_t rate = 48000 - output;
			printk("[CTRL] rate=%d lvl=%u filt=%u err=%d "
			       "P=%d I=%d out=%d\n",
			       rate, level, filtered,
			       error_bytes, p_out,
			       (int32_t)*i_sum, output);
		}
		#endif
	}

	return output;
}
