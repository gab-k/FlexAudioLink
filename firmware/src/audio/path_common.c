#include "audio/path_common.h"

#include <stdint.h>

#include <zephyr/logging/log.h>

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

int32_t codec_clock_controller(uint32_t target,
			       float *filter, float *i_sum,
			       float gain_mult, float ki,
			       uint32_t fifo, uint32_t pending)
{
	uint32_t level = fifo + pending;
	uint32_t filtered;
	int32_t error_bytes;
	int32_t p_out;
	int32_t output;

	if (filter == NULL || i_sum == NULL) {
		return 0;
	}

	filtered = filter_update(filter, level);
	error_bytes = (int32_t)target - (int32_t)filtered;

	p_out = (int32_t)((float)p_controller_step(error_bytes) * gain_mult);
	if (p_out > P_TERM_MAX_HZ) {
		p_out = P_TERM_MAX_HZ;
	} else if (p_out < -P_TERM_MAX_HZ) {
		p_out = -P_TERM_MAX_HZ;
	}

	*i_sum += (float)error_bytes * ki;

	if (*i_sum > (float)I_MAX_HZ) {
		*i_sum = (float)I_MAX_HZ;
	} else if (*i_sum < -(float)I_MAX_HZ) {
		*i_sum = -(float)I_MAX_HZ;
	}

	output = p_out + (int32_t)*i_sum;

	if (output > P_ADJUST_MAX_HZ) {
		output = P_ADJUST_MAX_HZ;
		if (error_bytes < 0) {
			*i_sum = (float)output - (float)p_out;
		}
	} else if (output < -P_ADJUST_MAX_HZ) {
		output = -P_ADJUST_MAX_HZ;
		if (error_bytes > 0) {
			*i_sum = (float)output - (float)p_out;
		}
	}

	{
		static uint32_t log_cnt = 0;

		#ifdef CTRL_DEBUG_LOG 
		if (++log_cnt % 10 == 0) {
			int32_t rate = 48000 - output;
			LOG_INF("rate=%d lvl=%u fifo=%u pend=%u filt=%u err=%d P=%d I=%d out=%d",
			       rate, level, fifo, pending, filtered,
			       error_bytes, p_out,
			       (int32_t)*i_sum, output);
		}
		#endif
	}

	return output;
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
