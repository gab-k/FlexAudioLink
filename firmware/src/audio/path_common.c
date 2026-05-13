#include "audio/path_common.h"

#include <stdint.h>

#include <zephyr/logging/log.h>

#include "audio/i2s.h"
#include "audio/nau88l21.h"
LOG_MODULE_REGISTER(path_cmn, LOG_LEVEL_INF);

const char *path_get_state_name(enum path_state state)
{
	switch (state) {
	case PATH_STATE_PLAYING:
		return "playing";
	case PATH_STATE_BUFFERING:
	default:
		return "buffering";
	}
}

static uint32_t filter_update(float *filtered, uint32_t level_bytes)
{
	if (filtered == NULL) {
		return 0U;
	}

	if (*filtered < 0.0f) {
		*filtered = (float)level_bytes;
	} else {
		const float alpha = (float)FILTER_ALPHA_NUM / (float)FILTER_ALPHA_DEN;

		*filtered = (alpha * (float)level_bytes) + ((1.0f - alpha) * *filtered);
	}

	return (uint32_t)*filtered;
}

static int32_t p_controller_step(int32_t error_bytes)
{
	if (error_bytes > P_ADJUST_MAX_HZ) {
		return P_ADJUST_MAX_HZ;
	}
	if (error_bytes < -P_ADJUST_MAX_HZ) {
		return -P_ADJUST_MAX_HZ;
	}

	return error_bytes;
}

void warn_on_level(uint32_t level, uint32_t fifo_bytes, uint32_t pending_bytes, uint32_t warn_low, uint32_t warn_high)
{
	static uint32_t last_low_warn_ms;
	static uint32_t last_high_warn_ms;

	if (level <= warn_low) {
		uint32_t now = k_uptime_get();
		if (now - last_low_warn_ms >= WARN_COOLDOWN_MS) {
			LOG_WRN("speaker level LOW %u B (fifo=%u pending=%u)", level, fifo_bytes, pending_bytes);
			last_low_warn_ms = now;
		}
	} else if (level >= warn_high) {
		uint32_t now = k_uptime_get();
		if (now - last_high_warn_ms >= WARN_COOLDOWN_MS) {
			LOG_WRN("speaker level HIGH %u B (fifo=%u pending=%u)", level, fifo_bytes, pending_bytes);
			last_high_warn_ms = now;
		}
	}
}

static int32_t codec_clock_controller(int32_t error_bytes)
{
	int32_t p_out;
	int32_t output;
	static float i_sum;

	p_out = (int32_t)((float)p_controller_step(error_bytes) * P_GAIN);
	if (p_out > P_TERM_MAX_HZ) {
		p_out = P_TERM_MAX_HZ;
	} else if (p_out < -P_TERM_MAX_HZ) {
		p_out = -P_TERM_MAX_HZ;
	}

	i_sum += (float)error_bytes * P_KI;

	if (i_sum > (float)I_MAX_HZ) {
		i_sum = (float)I_MAX_HZ;
	} else if (i_sum < -(float)I_MAX_HZ) {
		i_sum = -(float)I_MAX_HZ;
	}

	output = p_out + (int32_t)i_sum;

	if (output > P_ADJUST_MAX_HZ) {
		output = P_ADJUST_MAX_HZ;
		if (error_bytes < 0) {
			i_sum = (float)output - (float)p_out;
		}
	} else if (output < -P_ADJUST_MAX_HZ) {
		output = -P_ADJUST_MAX_HZ;
		if (error_bytes > 0) {
			i_sum = (float)output - (float)p_out;
		}
	}

	return output;
}

void update_codec_clock(uint32_t target,
			uint32_t fifo, uint32_t pending,
			uint32_t *filtered_level_bytes,
			int32_t *error_bytes,
			int32_t *p_adjust_hz,
			int32_t *fll_target_rate_hz)
{
	static uint32_t last_update_uptime_ms;
	static float filter = -1.0f;
	uint32_t now_ms;
	uint32_t level;
	uint32_t filtered;
	int32_t adjust_hz;
	int32_t target_rate;
	int ret;

	if (fll.fixed || filtered_level_bytes == NULL || error_bytes == NULL ||
	    p_adjust_hz == NULL || fll_target_rate_hz == NULL) {
		return;
	}

	now_ms = k_uptime_get();
	if (now_ms - last_update_uptime_ms < FLL_UPDATE_INTERVAL_MS) {
		return;
	}
	last_update_uptime_ms = now_ms;

	level = fifo + pending;
	filtered = filter_update(&filter, level);

	*filtered_level_bytes = filtered;
	*error_bytes = (int32_t)target - (int32_t)filtered;
	adjust_hz = codec_clock_controller(*error_bytes);
	*p_adjust_hz = adjust_hz;

	target_rate = (int32_t)AUDIO_I2S_SAMPLE_RATE_HZ - adjust_hz;
	ret = nau88l21_set_fll_target_rate_hz(target_rate);
	if (ret == 0) {
		*fll_target_rate_hz = target_rate;
	} else {
		LOG_ERR("Failed to set codec FLL target rate to %d Hz", target_rate);
	}

	#ifdef CTRL_DEBUG_LOG
	{
		static uint32_t log_cnt;

		if (++log_cnt % 10 == 0U) {
			LOG_INF("rate=%d lvl=%u fifo=%u pend=%u filt=%u err=%d out=%d",
				target_rate, level, fifo, pending, filtered,
				*error_bytes, adjust_hz);
		}
	}
	#endif
}

struct fll_state fll;

bool fll_set_fixed(int32_t rate_hz)
{
	if (nau88l21_set_fll_target_rate_hz(rate_hz) == 0) {
		fll.fixed = true;
		fll.fixed_rate_hz = rate_hz;
		return true;
	}
	return false;
}

void fll_set_auto(void)
{
	fll.fixed = false;
	fll.fixed_rate_hz = 0;
}

int32_t fll_get_fixed_rate(void)
{
	return fll.fixed ? fll.fixed_rate_hz : 0;
}
