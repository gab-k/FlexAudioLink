#include "audio_io/audio_path_wired.h"

#include <string.h>

#include <zephyr/kernel.h>

#include "audio_io/audio_path_common.h"
#include "audio_io/i2s.h"
#include "tusb.h"
#include "usb/usb_audio.h"

#define WIRED_THREAD_STACK_SIZE  3072
#define WIRED_THREAD_PRIORITY    7
#define WIRED_LOOP_SLEEP_MS      1
#define WIRED_FEEDBACK_GAIN_DIV  20

/* Wakes the parked wired worker on (re)activation. */
static K_SEM_DEFINE(g_wired_start_sem, 0, 1);
/* Acknowledges deactivate() after worker teardown. */
static K_SEM_DEFINE(g_wired_stop_sem, 0, 1);
static struct audio_path_wired_status g_wired_status;

static void wired_reset_state(void)
{
	memset(&g_wired_status, 0, sizeof(g_wired_status));
	g_wired_status.stream_state = AUDIO_PATH_STATE_BUFFERING;
	audio_i2s_tx_flush();
	audio_i2s_rx_flush();
	usb_audio_reset();
}

static void wired_send_i2s_to_mic_ep(void)
{
	while (1) {
		struct audio_i2s_block block;
		uint8_t mono[AUDIO_I2S_BLOCK_BYTES / 2U];
		size_t mono_bytes;
		size_t pushed;

		if (audio_i2s_rx_dequeue_block(&block, K_NO_WAIT) != 0) {
			break;
		}

		mono_bytes = audio_extract_left_to_mono(block.bytes, AUDIO_I2S_BLOCK_BYTES, mono, sizeof(mono));
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

static void wired_send_spk_ep_to_i2s(tu_fifo_t *spk_ff)
{
	/* Rate-limit per-tick so a BUFFERING->PLAYING transition can't drain
	 * the whole tu_fifo into tx_msgq in one burst. I2S consumes 1 block/ms;
	 * allow up to 2 to let us catch up on a missed tick but no more. */
	const unsigned int max_per_tick = 2U;

	if (g_wired_status.stream_state != AUDIO_PATH_STATE_PLAYING || spk_ff == NULL) {
		return;
	}

	/* Gate on pending: once we're ahead of target there's no reason
	 * to keep enqueuing — let the feedback loop close the gap via the host. */
	for (unsigned int i = 0; i < max_per_tick; i++) {
		if (audio_i2s_tx_get_pending_bytes() >= AUDIO_TARGET_BYTES) {
			break;
		}
		if (audio_i2s_tx_enqueue_fifo(spk_ff) != 0) {
			break;
		}
	}
}

static void wired_update_feedback(void)
{
	int32_t adjust_hz = g_wired_status.spk_error_bytes / WIRED_FEEDBACK_GAIN_DIV;
	int32_t rate_hz;

	if (adjust_hz > AUDIO_P_ADJUST_MAX_HZ) {
		adjust_hz = AUDIO_P_ADJUST_MAX_HZ;
	} else if (adjust_hz < -AUDIO_P_ADJUST_MAX_HZ) {
		adjust_hz = -AUDIO_P_ADJUST_MAX_HZ;
	}

	rate_hz = (int32_t)AUDIO_I2S_SAMPLE_RATE_HZ + adjust_hz;

	g_wired_status.spk_p_adjust_hz = adjust_hz;
	(void)tud_audio_fb_set((uint32_t)(((uint64_t)rate_hz << 16) / 8000ULL));
}

void audio_path_wired_activate(void)
{
	if (g_wired_status.active) {
		return;
	}

	wired_reset_state();
	g_wired_status.active = true;
	k_sem_give(&g_wired_start_sem);
}

void audio_path_wired_deactivate(void)
{
	if (!g_wired_status.active) {
		return;
	}

	g_wired_status.active = false;
	(void)k_sem_take(&g_wired_stop_sem, K_FOREVER);
}

void audio_path_wired_get_status(struct audio_path_wired_status *out)
{
	if (out == NULL) {
		return;
	}

	*out = g_wired_status;
}

static void wired_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (1) {
		/* Park until activate() selects the wired path. */
		(void)k_sem_take(&g_wired_start_sem, K_FOREVER);

		/* EMA filter state; negative means uninitialized (first sample seeds). */
		float filter = -1.0f;

		while (g_wired_status.active) {
			tu_fifo_t *spk_ff = tud_audio_get_ep_out_ff();
			uint32_t fifo_bytes = (spk_ff != NULL) ? tu_fifo_count(spk_ff) : 0U;
			uint32_t pending = audio_i2s_tx_get_pending_bytes();
			uint32_t level = fifo_bytes + pending;
			const uint32_t target = AUDIO_TARGET_BYTES;

			if (filter < 0.0f) {
				filter = (float)level;
			} else {
				const float alpha = (float)AUDIO_FILTER_ALPHA_NUM /
						    (float)AUDIO_FILTER_ALPHA_DEN;

				filter = alpha * (float)level + (1.0f - alpha) * filter;
			}

			g_wired_status.spk_level_bytes = level;
			g_wired_status.spk_fifo_bytes = fifo_bytes;
			g_wired_status.spk_pending_bytes = pending;
			g_wired_status.spk_filtered_level_bytes = (uint32_t)filter;
			g_wired_status.spk_error_bytes =
				(int32_t)target - (int32_t)g_wired_status.spk_filtered_level_bytes;

			/* BUFFERING -> PLAYING at AUDIO_START_BYTES,
			 * PLAYING -> BUFFERING on empty. */
			if (g_wired_status.stream_state == AUDIO_PATH_STATE_BUFFERING) {
				if (spk_ff != NULL && level >= AUDIO_START_BYTES) {
					g_wired_status.stream_state = AUDIO_PATH_STATE_PLAYING;
				}
			} else if (level == 0U) {
				g_wired_status.stream_state = AUDIO_PATH_STATE_BUFFERING;
			}

			wired_send_spk_ep_to_i2s(spk_ff);
			wired_update_feedback();

			wired_send_i2s_to_mic_ep();
			g_wired_status.mic_level_bytes = usb_audio_microphone_level_bytes();

			k_sleep(K_MSEC(WIRED_LOOP_SLEEP_MS));
		}

		/* Tell deactivate() that the worker has exited. */
		k_sem_give(&g_wired_stop_sem);
	}
}

K_THREAD_DEFINE(wired_thread_id, WIRED_THREAD_STACK_SIZE, wired_thread, NULL, NULL, NULL, WIRED_THREAD_PRIORITY, 0, 0);
