#include "audio/path_wired.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(path_wired, LOG_LEVEL_INF);

#include "audio/path_common.h"
#include "audio/i2s.h"

#define WIRED_THREAD_STACK_SIZE        3072
#define WIRED_THREAD_PRIORITY          7
#define WIRED_LOOP_SLEEP_MS            1

#define WIRED_START_BYTES            480U
#define WIRED_TARGET_BYTES           480U
#define WIRED_WARN_LOW_BYTES  (WIRED_TARGET_BYTES * 10U / 50U )
#define WIRED_WARN_HIGH_BYTES (WIRED_TARGET_BYTES * 90U / 50U )

static struct codec_path_status wired_status;

static K_THREAD_STACK_DEFINE(wired_thread_stack, WIRED_THREAD_STACK_SIZE);
static struct k_thread wired_thread_data;

static void wired_thread(void *a, void *b, void *c);

void path_wired_init(void)
{
	wired_status.stream_state = PATH_STATE_BUFFERING;
	wired_status.spk_fll_target_rate_hz = (int32_t)AUDIO_I2S_SAMPLE_RATE_HZ;

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

	*out = wired_status;
	if (fll.fixed) {
		out->spk_fll_target_rate_hz = fll.fixed_rate_hz;
	}
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

		wired_status.spk_fifo_bytes = ep_out_ff_bytes;
		wired_status.spk_pending_bytes = pending;

		if (wired_status.stream_state == PATH_STATE_BUFFERING) {
			if (level >= WIRED_START_BYTES) {
				wired_status.stream_state = PATH_STATE_PLAYING;
				LOG_INF("switching to PLAYING, notifying i2s thread...");
				audio_i2s_activate(ep_out_ff, ep_in_ff);
			}
		} else if (pending == 0U && ep_out_ff_bytes < AUDIO_I2S_BLOCK_BYTES) {
			wired_status.stream_state = PATH_STATE_BUFFERING;
			LOG_INF("switching to BUFFERING, notifying i2s thread...");
			audio_i2s_deactivate();
			wired_status.spk_underrun_events++;
		}

		if (wired_status.stream_state == PATH_STATE_PLAYING) {
			update_codec_clock(WIRED_TARGET_BYTES,
					   ep_out_ff_bytes, pending,
					   &wired_status);
			#ifdef WARN_SPK_LVL
			warn_on_level(level, ep_out_ff_bytes, pending, WIRED_WARN_LOW_BYTES, WIRED_WARN_HIGH_BYTES);
			#endif
		}

		k_sleep(K_MSEC(WIRED_LOOP_SLEEP_MS));
	}
}
