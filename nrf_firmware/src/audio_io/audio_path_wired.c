#include "audio_io/audio_path_wired.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(audio_path_wired, LOG_LEVEL_INF);

#include "audio_io/audio_path_common.h"
#include "audio_io/i2s.h"
#include "audio_io/nau88l21.h"
#include "tusb.h"
#include "usb/usb_audio.h"

#define WIRED_THREAD_STACK_SIZE        3072
#define WIRED_THREAD_PRIORITY          7
#define WIRED_LOOP_SLEEP_MS            1

#define WIRED_WARN_LOW_BYTES \
	((AUDIO_TARGET_BYTES / 50U) * 10U)   /*  96 bytes */
#define WIRED_WARN_HIGH_BYTES \
	((AUDIO_TARGET_BYTES / 50U) * 90U)  /* 864 bytes */
#define WIRED_WARN_COOLDOWN_MS          2000

/* Uncomment to print speaker buffer-level warnings every WARN_COOLDOWN_MS. */
#define WIRED_SPK_LEVEL_WARN

/* Wakes the parked wired worker on (re)activation. */
static K_SEM_DEFINE(g_wired_start_sem, 0, 1);
/* Acknowledges deactivate() after worker teardown. */
static K_SEM_DEFINE(g_wired_stop_sem, 0, 1);
static struct audio_path_wired_status g_wired_status;
static bool g_wired_fll_fixed;
static int32_t g_wired_fll_fixed_rate_hz;

/* Mic FIFO: I2S writes stereo blocks here; the wired thread reads,
 * extracts left-mono, and forwards to USB. */
#define WIRED_RX_FIFO_SIZE  4096
static uint8_t g_wired_rx_fifo_buf[WIRED_RX_FIFO_SIZE];
static tu_fifo_t g_wired_rx_fifo;
static OSAL_MUTEX_DEF(g_wired_rx_mutex_wr);
static OSAL_MUTEX_DEF(g_wired_rx_mutex_rd);

static void wired_reset_state(void)
{
	memset(&g_wired_status, 0, sizeof(g_wired_status));
	g_wired_status.stream_state = AUDIO_PATH_STATE_BUFFERING;
	g_wired_status.spk_fll_target_rate_hz = (int32_t)AUDIO_I2S_SAMPLE_RATE_HZ;
	audio_i2s_deactivate();
	tu_fifo_clear(&g_wired_rx_fifo);
	usb_audio_reset();
}

static void wired_send_i2s_to_mic_ep(void)
{
	while (1) {
		uint8_t stereo[AUDIO_I2S_BLOCK_BYTES];
		uint8_t mono[AUDIO_I2S_BLOCK_BYTES / 2U];
		size_t mono_bytes;
		size_t pushed;

		if (tu_fifo_read_n(&g_wired_rx_fifo, stereo,
				   AUDIO_I2S_BLOCK_BYTES) <
		    AUDIO_I2S_BLOCK_BYTES) {
			break;
		}

		mono_bytes = audio_extract_left_to_mono(stereo,
			AUDIO_I2S_BLOCK_BYTES, mono, sizeof(mono));
		if (mono_bytes == 0U) {
			g_wired_status.mic_overflow_bytes += AUDIO_I2S_BLOCK_BYTES;
			continue;
		}

		pushed = usb_audio_write_microphone_bytes(mono, mono_bytes);
		if (pushed < mono_bytes) {
			g_wired_status.mic_overflow_bytes += mono_bytes - pushed;
		}
	}
}

static void wired_update_codec_clock(uint32_t level)
{
	static uint32_t last_update_uptime_ms;
	static float filter = -1.0f;
	static float i_sum;
	uint32_t now_ms;

	now_ms = k_uptime_get();
	if (now_ms - last_update_uptime_ms < AUDIO_FLL_UPDATE_INTERVAL_MS) {
		return;
	}
	last_update_uptime_ms = now_ms;

	int32_t adjust_hz = audio_codec_clock_controller(level, AUDIO_TARGET_BYTES,
							  &filter, &i_sum,
							  AUDIO_P_GAIN, AUDIO_P_KI);

	g_wired_status.spk_filtered_level_bytes = (uint32_t)filter;
	g_wired_status.spk_error_bytes = (int32_t)AUDIO_TARGET_BYTES - (int32_t)g_wired_status.spk_filtered_level_bytes;
	g_wired_status.spk_p_adjust_hz = adjust_hz;

	{
		int32_t target_rate = (int32_t)AUDIO_I2S_SAMPLE_RATE_HZ - adjust_hz;
		int ret = nau88l21_set_fll_target_rate_hz(target_rate);

		if (ret == 0) {
			g_wired_status.spk_fll_target_rate_hz = target_rate;
		} else {
			g_wired_status.spk_fll_fails++;
		}
	}
}

void audio_path_wired_activate(void)
{
	if (g_wired_status.active) {
		return;
	}

	wired_reset_state();
	tu_fifo_config(&g_wired_rx_fifo, g_wired_rx_fifo_buf,
		       WIRED_RX_FIFO_SIZE, 1);
	tu_fifo_config_mutex(&g_wired_rx_fifo,
			     &g_wired_rx_mutex_wr, &g_wired_rx_mutex_rd);
	g_wired_status.active = true;
	k_sem_give(&g_wired_start_sem);
}

void audio_path_wired_deactivate(void)
{
	if (!g_wired_status.active) {
		return;
	}

	g_wired_status.active = false;
	audio_i2s_deactivate();
	(void)k_sem_take(&g_wired_stop_sem, K_FOREVER);
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

	while (1) {
		/* Park until activate() selects the wired path. */
		(void)k_sem_take(&g_wired_start_sem, K_FOREVER);

		while (g_wired_status.active) {
			tu_fifo_t *spk_ff = tud_audio_get_ep_out_ff();
			uint32_t fifo_bytes = (spk_ff != NULL) ? tu_fifo_count(spk_ff) : 0U;
			uint32_t pending = audio_i2s_tx_get_pending_bytes();
			uint32_t level = fifo_bytes + pending;

			g_wired_status.spk_level_bytes = level;
			g_wired_status.spk_fifo_bytes = fifo_bytes;
			g_wired_status.spk_pending_bytes = pending;

			if (g_wired_status.stream_state == AUDIO_PATH_STATE_BUFFERING) {
				if (level >= AUDIO_START_BYTES) {
					g_wired_status.stream_state = AUDIO_PATH_STATE_PLAYING;
					LOG_INF("switching to PLAYING, notifying i2s thread...");
					audio_i2s_activate(spk_ff, &g_wired_rx_fifo);
				}
			} else if (level == 0U) {
				g_wired_status.stream_state = AUDIO_PATH_STATE_BUFFERING;
				LOG_INF("switching to BUFFERING, notifying i2s thread...");
				audio_i2s_deactivate();
				g_wired_status.spk_underrun_events++;
			}
		
			#ifdef WIRED_SPK_LEVEL_WARN
			if (g_wired_status.stream_state == AUDIO_PATH_STATE_PLAYING) {
				static uint32_t last_low_warn_ms;
				static uint32_t last_high_warn_ms;

				if (level <= WIRED_WARN_LOW_BYTES) {
					uint32_t now = k_uptime_get();
					if (now - last_low_warn_ms >= WIRED_WARN_COOLDOWN_MS) {
						LOG_WRN("speaker level LOW %u B (fifo=%u pending=%u)", level, fifo_bytes, pending);
						last_low_warn_ms = now;
					}
				} else if (level >= WIRED_WARN_HIGH_BYTES) {
					uint32_t now = k_uptime_get();
					if (now - last_high_warn_ms >= WIRED_WARN_COOLDOWN_MS) {
						LOG_WRN("speaker level HIGH %u B (fifo=%u pending=%u)", level, fifo_bytes, pending);
						last_high_warn_ms = now;
					}
				}
			}
			#endif

			if (!g_wired_fll_fixed &&
			    g_wired_status.stream_state == AUDIO_PATH_STATE_PLAYING) {
				wired_update_codec_clock(level);
			}

			wired_send_i2s_to_mic_ep();
			g_wired_status.mic_level_bytes = usb_audio_microphone_level_bytes();

			k_sleep(K_MSEC(WIRED_LOOP_SLEEP_MS));
		}

		/* Tell deactivate() that the worker has exited. */
		k_sem_give(&g_wired_stop_sem);
	}
}

K_THREAD_DEFINE(wired_thread_id, WIRED_THREAD_STACK_SIZE, wired_thread, NULL, NULL, NULL, WIRED_THREAD_PRIORITY, 0, 0);
