#include "audio_io/audio_path_wired.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(audio_path_wired, LOG_LEVEL_INF);

#include "audio_io/audio_path_common.h"
#include "audio_io/i2s.h"
#include "audio_io/nau88l21.h"

#define WIRED_THREAD_STACK_SIZE        3072
#define WIRED_THREAD_PRIORITY          7
#define WIRED_LOOP_SLEEP_MS            1

#define WIRED_AUDIO_START_BYTES            480U
#define WIRED_AUDIO_TARGET_BYTES           480U
#define WIRED_WARN_LOW_BYTES  (WIRED_AUDIO_TARGET_BYTES * 10U / 50U )
#define WIRED_WARN_HIGH_BYTES (WIRED_AUDIO_TARGET_BYTES * 90U / 50U )

static struct audio_path_wired_status g_wired_status;
static bool g_wired_fll_fixed;
static int32_t g_wired_fll_fixed_rate_hz;

static K_THREAD_STACK_DEFINE(g_wired_thread_stack, WIRED_THREAD_STACK_SIZE);
static struct k_thread g_wired_thread;

static void wired_thread(void *a, void *b, void *c);

static void wired_update_codec_clock(uint32_t fifo_lvl, uint32_t pending)
{
	static uint32_t last_ms;
	static float filter = -1.0f;
	static float i_sum;
	uint32_t now_ms;

	if (g_wired_fll_fixed) {
		return;
	}

	now_ms = k_uptime_get();
	if (now_ms - last_ms < AUDIO_FLL_UPDATE_INTERVAL_MS) {
		return;
	}
	last_ms = now_ms;

	int32_t adjust_hz = audio_codec_clock_controller(WIRED_AUDIO_TARGET_BYTES,
							  &filter, &i_sum,
							  AUDIO_P_GAIN, AUDIO_P_KI,
							  fifo_lvl, pending);

	g_wired_status.spk_filtered_level_bytes = (uint32_t)filter;
	g_wired_status.spk_error_bytes = (int32_t)WIRED_AUDIO_TARGET_BYTES - (int32_t)g_wired_status.spk_filtered_level_bytes;
	g_wired_status.spk_p_adjust_hz = adjust_hz;

	int32_t target_rate = (int32_t)AUDIO_I2S_SAMPLE_RATE_HZ - adjust_hz;
	int ret = nau88l21_set_fll_target_rate_hz(target_rate);

	if (ret == 0) {
		g_wired_status.spk_fll_target_rate_hz = target_rate;
	} else {
		LOG_ERR("Failed to set codec FLL target rate to %d Hz", target_rate);
	}
}

void audio_path_wired_init(void)
{
	g_wired_status.stream_state = AUDIO_PATH_STATE_BUFFERING;
	g_wired_status.spk_fll_target_rate_hz = (int32_t)AUDIO_I2S_SAMPLE_RATE_HZ;

	k_thread_create(&g_wired_thread, g_wired_thread_stack,
			K_THREAD_STACK_SIZEOF(g_wired_thread_stack),
			wired_thread, NULL, NULL, NULL,
			WIRED_THREAD_PRIORITY, 0, K_NO_WAIT);
}

void audio_path_wired_get_status(struct audio_path_wired_status *out)
{
	if (out == NULL) {
		return;
	}

	*out = g_wired_status;
}

void audio_path_wired_fll_set_fixed(int32_t rate_hz)
{
	if (nau88l21_set_fll_target_rate_hz(rate_hz) == 0) {
		g_wired_status.spk_fll_target_rate_hz = rate_hz;
		g_wired_fll_fixed = true;
		g_wired_fll_fixed_rate_hz = rate_hz;
	}
}

void audio_path_wired_fll_set_auto(void)
{
	g_wired_fll_fixed = false;
	g_wired_fll_fixed_rate_hz = 0;
}

int32_t audio_path_wired_fll_get_fixed_rate(void)
{
	return g_wired_fll_fixed ? g_wired_fll_fixed_rate_hz : 0;
}

static void wired_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	tu_fifo_t *ep_out_ff = NULL;
	tu_fifo_t *ep_in_ff = NULL;

	while (ep_out_ff == NULL || ep_in_ff == NULL) {
		ep_out_ff = tud_audio_get_ep_out_ff();
		ep_in_ff = tud_audio_get_ep_in_ff();
		k_sleep(K_MSEC(WIRED_LOOP_SLEEP_MS));
	}

	while (1) {
		uint32_t ep_out_ff_bytes = tu_fifo_count(ep_out_ff);
		uint32_t pending = audio_i2s_tx_get_pending_bytes();
		uint32_t level = ep_out_ff_bytes + pending;

		g_wired_status.spk_fifo_bytes = ep_out_ff_bytes;
		g_wired_status.spk_pending_bytes = pending;

		if (g_wired_status.stream_state == AUDIO_PATH_STATE_BUFFERING) {
			if (level >= WIRED_AUDIO_START_BYTES) {
				g_wired_status.stream_state = AUDIO_PATH_STATE_PLAYING;
				LOG_INF("switching to PLAYING, notifying i2s thread...");
				audio_i2s_activate(ep_out_ff, ep_in_ff);
			}
		} else if (pending == 0U && ep_out_ff_bytes < AUDIO_I2S_BLOCK_BYTES) {
			g_wired_status.stream_state = AUDIO_PATH_STATE_BUFFERING;
			LOG_INF("switching to BUFFERING, notifying i2s thread...");
			audio_i2s_deactivate();
			g_wired_status.spk_underrun_events++;
		}

		if (g_wired_status.stream_state == AUDIO_PATH_STATE_PLAYING) {
			wired_update_codec_clock(ep_out_ff_bytes, pending);
			#ifdef WARN_SPK_LVL
			warn_on_level(level, ep_out_ff_bytes, pending, WIRED_WARN_LOW_BYTES, WIRED_WARN_HIGH_BYTES);
			#endif
		}

		k_sleep(K_MSEC(WIRED_LOOP_SLEEP_MS));
	}
}
