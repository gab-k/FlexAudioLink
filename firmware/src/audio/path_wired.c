#include "audio/path_wired.h"
#include "audio/path_common.h"
#include "audio/i2s.h"
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(path_wired, LOG_LEVEL_INF);

#define WIRED_THREAD_STACK_SIZE        3072
#define WIRED_THREAD_PRIORITY          7
#define WIRED_LOOP_SLEEP_MS            1

#define WIRED_START_BYTES            480U
#define WIRED_TARGET_BYTES           480U

static struct codec_path_status status;

K_SEM_DEFINE(wired_usb_audio_rx_sem, 0, 1);

static K_THREAD_STACK_DEFINE(wired_thread_stack, WIRED_THREAD_STACK_SIZE);
static struct k_thread wired_thread_data;

static void wired_thread(void *a, void *b, void *c);

void path_wired_init(void)
{
	status.stream_state = PATH_STATE_BUFFERING;
	status.spk_fll_target_rate_hz = (int32_t)AUDIO_I2S_SAMPLE_RATE_HZ;

	k_thread_create(&wired_thread_data, wired_thread_stack,
			K_THREAD_STACK_SIZEOF(wired_thread_stack),
			wired_thread, NULL, NULL, NULL,
			WIRED_THREAD_PRIORITY, 0, K_NO_WAIT);
}

void path_wired_get_status(struct codec_path_status *out)
{
	if (out == NULL) {
		return;
	}

	*out = status;
	if (fll.fixed) {
		out->spk_fll_target_rate_hz = fll.fixed_rate_hz;
	}
}

void path_wired_wake_thread(void)
{
	k_sem_give(&wired_usb_audio_rx_sem);
}

static void wired_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	tu_fifo_t *ep_out_ff = NULL;
	tu_fifo_t *ep_in_ff = NULL;
	uint32_t now_ms;

	while (ep_out_ff == NULL || ep_in_ff == NULL) {
		ep_out_ff = tud_audio_get_ep_out_ff();
		ep_in_ff = tud_audio_get_ep_in_ff();
		k_sleep(K_MSEC(WIRED_LOOP_SLEEP_MS));
	}

	while (1) {
		status.spk_fifo_bytes = tu_fifo_count(ep_out_ff);
		status.spk_pending_bytes = audio_i2s_tx_get_pending_bytes();
		uint32_t level = status.spk_fifo_bytes + status.spk_pending_bytes;

		if (status.stream_state == PATH_STATE_BUFFERING) {
			if (level >= WIRED_START_BYTES) {
				status.stream_state = PATH_STATE_PLAYING;
				LOG_INF("switching to PLAYING, notifying i2s thread...");
				audio_i2s_activate(ep_out_ff, ep_in_ff);
			}
		} else if (status.spk_pending_bytes == 0U && status.spk_fifo_bytes < AUDIO_I2S_BLOCK_BYTES) {
			status.stream_state = PATH_STATE_BUFFERING;
			LOG_INF("switching to BUFFERING, notifying i2s thread...");
			audio_i2s_deactivate();
			status.spk_underrun_events++;
		}

		if (status.stream_state == PATH_STATE_PLAYING) {
			static uint32_t last_controller_ms;
			static uint32_t last_filter_ms;
			static float filter = -1.0f;

			now_ms = k_uptime_get();
			if (now_ms - last_filter_ms >= EMA_FILTER_UPDATE_INTERVAL_MS) {
				status.spk_filtered_level_bytes = codec_level_filter_update(&filter, level);
				last_filter_ms = now_ms;
			}

			now_ms = k_uptime_get();
			if (now_ms - last_controller_ms >= FLL_UPDATE_INTERVAL_MS && !fll.fixed) {
				codec_clock_controller(&status, WIRED_TARGET_BYTES,
						       AUDIO_I2S_SAMPLE_RATE_HZ);
				last_controller_ms = now_ms;
			}
		}

		k_sem_take(&wired_usb_audio_rx_sem, K_MSEC(WIRED_LOOP_SLEEP_MS));
	}
}
