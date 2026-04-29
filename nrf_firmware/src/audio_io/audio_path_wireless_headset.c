#include "audio_io/audio_path_wireless_headset.h"

#include <string.h>

#include <zephyr/kernel.h>

#include "audio_io/audio_path_common.h"
#include "audio_io/i2s.h"
#include "audio_io/nau88l21.h"
#include "prop_gfsk/link.h"
#include "prop_gfsk/test_mode.h"
#include "tusb.h"

#define HEADSET_THREAD_STACK_SIZE  3072
#define HEADSET_THREAD_PRIORITY    6
#define HEADSET_LOOP_SLEEP_MS      1

/* Speaker FIFO: reuse USB audio EP OUT FIFO (tud_audio_get_ep_out_ff()).
 * Mic FIFO: reuse USB audio EP IN FIFO (tud_audio_get_ep_in_ff()).
 * The USB audio class configures these statically; we add mutex protection
 * in usb_device_start() after tusb_init(). */

static struct audio_path_wireless_status g_headset_status;
static bool g_headset_fll_fixed;
static int32_t g_headset_fll_fixed_rate_hz;

static K_THREAD_STACK_DEFINE(g_headset_thread_stack, HEADSET_THREAD_STACK_SIZE);
static struct k_thread g_headset_thread;

static void headset_thread(void *a, void *b, void *c);

void audio_path_wireless_headset_init(void)
{
	g_headset_status.stream_state = AUDIO_PATH_STATE_BUFFERING;
	g_headset_status.spk_fll_target_rate_hz = (int32_t)AUDIO_I2S_SAMPLE_RATE_HZ;

	k_thread_create(&g_headset_thread, g_headset_thread_stack,
			K_THREAD_STACK_SIZEOF(g_headset_thread_stack),
			headset_thread, NULL, NULL, NULL,
			HEADSET_THREAD_PRIORITY, 0, K_NO_WAIT);
}

void audio_path_wireless_headset_get_status(struct audio_path_wireless_status *out)
{
	if (out != NULL) {
		*out = g_headset_status;
	}
}

void audio_path_wireless_headset_fll_set_fixed(int32_t rate_hz)
{
	if (nau88l21_set_fll_target_rate_hz(rate_hz) == 0) {
		g_headset_status.spk_fll_target_rate_hz = rate_hz;
		g_headset_fll_fixed = true;
		g_headset_fll_fixed_rate_hz = rate_hz;
	}
}

void audio_path_wireless_headset_fll_set_auto(void)
{
	g_headset_fll_fixed = false;
	g_headset_fll_fixed_rate_hz = 0;
}

int32_t audio_path_wireless_headset_fll_get_fixed_rate(void)
{
	return g_headset_fll_fixed ? g_headset_fll_fixed_rate_hz : 0;
}

static void headset_update_codec_clock(uint32_t fifo, uint32_t pending)
{
	if (g_headset_fll_fixed) {
		return;
	}

	static uint32_t last_update_uptime_ms;
	static float filter = -1.0f;
	static float i_sum;
	uint32_t now_ms;

	now_ms = k_uptime_get();
	if (now_ms - last_update_uptime_ms < AUDIO_FLL_UPDATE_INTERVAL_MS) {
		return;
	}
	last_update_uptime_ms = now_ms;

	int32_t adjust_hz = audio_codec_clock_controller(
		AUDIO_TARGET_BYTES, &filter, &i_sum,
		AUDIO_P_GAIN, AUDIO_P_KI,
		fifo, pending);

	g_headset_status.spk_filtered_level_bytes = (uint32_t)filter;
	g_headset_status.spk_p_adjust_hz = adjust_hz;

	int32_t target_rate = (int32_t)AUDIO_I2S_SAMPLE_RATE_HZ - adjust_hz;
	int ret = nau88l21_set_fll_target_rate_hz(target_rate);

	if (ret == 0) {
		g_headset_status.spk_fll_target_rate_hz = target_rate;
	} else {
		g_headset_status.spk_fll_fails++;
	}
}

static void headset_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (1) {
		/* 1. PGFSK RX → speaker FIFO */
		while (1) {
			struct pgfsk_frame frame;

			if (!pgfsk_link_rx_dequeue(&frame, K_NO_WAIT)) {
				break;
			}

			if (frame.len < AUDIO_I2S_BLOCK_BYTES) {
				g_headset_status.spk_silence_inserted_bytes += AUDIO_I2S_BLOCK_BYTES - frame.len;
				continue;
			}

			uint32_t written = tu_fifo_write_n( tud_audio_get_ep_out_ff(), frame.payload, AUDIO_I2S_BLOCK_BYTES);
			if (written < AUDIO_I2S_BLOCK_BYTES) {
				g_headset_status.overflow_bytes += AUDIO_I2S_BLOCK_BYTES - written;
			}
		}

		/* 2. I2S RX → PGFSK TX (if link in service) */
		if (pgfsk_link_get_state() == PGFSK_LINK_STATE_IN_SERVICE) {
			while (1) {
				uint8_t stereo[AUDIO_I2S_BLOCK_BYTES];
				struct pgfsk_frame frame;
				size_t mono_bytes;

				if (tu_fifo_read_n(
					    tud_audio_get_ep_in_ff(),
					    stereo,
					    AUDIO_I2S_BLOCK_BYTES) <
				    AUDIO_I2S_BLOCK_BYTES) {
					break;
				}

				memset(&frame, 0, sizeof(frame));
				mono_bytes = audio_extract_left_to_mono(
					stereo,
					AUDIO_I2S_BLOCK_BYTES,
					frame.payload,
					PGFSK_PAYLOAD_MAX_LEN);
				if (mono_bytes == 0U) {
					g_headset_status.overflow_bytes += AUDIO_I2S_BLOCK_BYTES;
					continue;
				}

				frame.len = mono_bytes;

				if (!pgfsk_link_tx_enqueue(&frame, K_NO_WAIT)) {
					if (pgfsk_link_get_state() == PGFSK_LINK_STATE_IN_SERVICE) {
						g_headset_status.overflow_bytes += mono_bytes;
					}
				}
			}
		}

		/* 3. State machine & FIFO management */
		uint32_t fifo_bytes = tu_fifo_count(tud_audio_get_ep_out_ff());
		uint32_t pending = audio_i2s_tx_get_pending_bytes();
		uint32_t level = fifo_bytes + pending;

		g_headset_status.spk_level_bytes = level;
		enum audio_path_state prev = g_headset_status.stream_state;
		g_headset_status.stream_state = audio_state_advance( g_headset_status.stream_state, level);

		if (prev != g_headset_status.stream_state) {
			printk("headset: %s\n", g_headset_status.stream_state == AUDIO_PATH_STATE_PLAYING ? "PLAYING" : "BUFFERING");

			if (g_headset_status.stream_state == AUDIO_PATH_STATE_PLAYING) {
				audio_i2s_activate(tud_audio_get_ep_out_ff(), tud_audio_get_ep_in_ff());
			} else {
				audio_i2s_deactivate();
			}
		}

		/* 4. FLL controller */
		if (g_headset_status.stream_state == AUDIO_PATH_STATE_PLAYING) {
			headset_update_codec_clock(fifo_bytes, pending);
		}

		/* 5. No USB mic on headset */
		g_headset_status.mic_usb_level_bytes = 0U;

		k_sleep(K_MSEC(HEADSET_LOOP_SLEEP_MS));
	}
}
