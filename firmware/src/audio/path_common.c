#include "audio/path_common.h"
#include <stdint.h>
#include "audio/nau88l21.h"
#include <zephyr/logging/log.h>
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

uint32_t codec_level_filter_update(float *filtered, uint32_t level_bytes)
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

void monitor_codec_level(const struct codec_path_status *status, uint32_t warn_low, uint32_t warn_high)
{
	uint32_t level;

	if (status == NULL) {
		return;
	}

	level = status->spk_fifo_bytes + status->spk_pending_bytes;

	#ifdef CTRL_DEBUG_LOG
	LOG_INF_RATELIMIT_RATE(CTRL_DEBUG_LOG_INTERVAL_MS,
				"rate=%d lvl=%u fifo=%u pend=%u filt=%u err=%d out=%d",
				status->spk_fll_target_rate_hz,
				level,
				status->spk_fifo_bytes,
				status->spk_pending_bytes,
				status->spk_filtered_level_bytes,
				status->spk_error_bytes,
				status->spk_p_adjust_hz);
	#endif

	#ifdef WARN_SPK_LVL
	if (level <= warn_low) {
		LOG_WRN_RATELIMIT_RATE(WARN_COOLDOWN_MS,
					"speaker level LOW %u B (fifo=%u pending=%u)",
					level,
					status->spk_fifo_bytes,
					status->spk_pending_bytes);
	} else if (level >= warn_high) {
		LOG_WRN_RATELIMIT_RATE(WARN_COOLDOWN_MS,
					"speaker level HIGH %u B (fifo=%u pending=%u)",
					level,
					status->spk_fifo_bytes,
					status->spk_pending_bytes);
	}
	#endif
}

int32_t codec_clock_controller(int32_t error_bytes, uint32_t nominal_rate_hz, int32_t *out_fll_target_rate_hz)
{
	int32_t p_out;
	int32_t output;
	static float i_sum;
	int ret;

	p_out = (int32_t)((float)error_bytes * P_GAIN);
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

	if (output > FLL_ADJUST_MAX_HZ) {
		output = FLL_ADJUST_MAX_HZ;
		if (error_bytes < 0) {
			i_sum = (float)output - (float)p_out;
		}
	} else if (output < -FLL_ADJUST_MAX_HZ) {
		output = -FLL_ADJUST_MAX_HZ;
		if (error_bytes > 0) {
			i_sum = (float)output - (float)p_out;
		}
	}

	ret = nau88l21_set_fll_target_rate_hz(nominal_rate_hz - output);
	if (ret) {
		LOG_ERR("Failed to set codec FLL target rate to %d Hz", nominal_rate_hz - output);
	} else {
		*out_fll_target_rate_hz = nominal_rate_hz - output;
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
