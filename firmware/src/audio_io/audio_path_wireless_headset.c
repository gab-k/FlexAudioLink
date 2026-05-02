#include "audio_io/audio_path_wireless_headset.h"

#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "audio_io/audio_path_common.h"
#include "audio_io/i2s.h"
#include "audio_io/nau88l21.h"
#include "prop_fsk/session.h"
#include "prop_fsk/test_mode.h"
#include "tusb.h"

#define HEADSET_THREAD_STACK_SIZE  3072
#define HEADSET_THREAD_PRIORITY    6
#define HEADSET_LOOP_SLEEP_MS      1
#define HEADSET_SPK_FIFO_SIZE      4096
#define HEADSET_MIC_FIFO_SIZE      4096

static struct audio_path_wireless_status g_headset_status;
static bool g_headset_fll_fixed;
static int32_t g_headset_fll_fixed_rate_hz;

static uint8_t g_headset_spk_fifo_buf[HEADSET_SPK_FIFO_SIZE];
static uint8_t g_headset_mic_fifo_buf[HEADSET_MIC_FIFO_SIZE];
static tu_fifo_t g_headset_spk_fifo;
static tu_fifo_t g_headset_mic_fifo;
static OSAL_MUTEX_DEF(g_headset_spk_mutex_wr);
static OSAL_MUTEX_DEF(g_headset_spk_mutex_rd);
static OSAL_MUTEX_DEF(g_headset_mic_mutex_wr);
static OSAL_MUTEX_DEF(g_headset_mic_mutex_rd);

static K_THREAD_STACK_DEFINE(g_headset_thread_stack, HEADSET_THREAD_STACK_SIZE);
static struct k_thread g_headset_thread;

static void headset_thread(void *a, void *b, void *c);

void audio_path_wireless_headset_init(void)
{
	g_headset_status = (struct audio_path_wireless_status){
		.stream_state = AUDIO_PATH_STATE_BUFFERING,
		.spk_fll_target_rate_hz = (int32_t)AUDIO_I2S_SAMPLE_RATE_HZ,
	};
	g_headset_fll_fixed = false;
	g_headset_fll_fixed_rate_hz = 0;

	tu_fifo_config(&g_headset_spk_fifo, g_headset_spk_fifo_buf,
		       HEADSET_SPK_FIFO_SIZE, true);
	tu_fifo_config_mutex(&g_headset_spk_fifo,
			     osal_mutex_create(&g_headset_spk_mutex_wr),
			     osal_mutex_create(&g_headset_spk_mutex_rd));
	tu_fifo_config(&g_headset_mic_fifo, g_headset_mic_fifo_buf,
		       HEADSET_MIC_FIFO_SIZE, true);
	tu_fifo_config_mutex(&g_headset_mic_fifo,
			     osal_mutex_create(&g_headset_mic_mutex_wr),
			     osal_mutex_create(&g_headset_mic_mutex_rd));

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
		if (pfsk_test_mode_is_running()) {
			if (g_headset_status.stream_state == AUDIO_PATH_STATE_PLAYING) {
				audio_i2s_deactivate();
			}
			g_headset_status.stream_state = AUDIO_PATH_STATE_BUFFERING;
			g_headset_status.spk_level_bytes = 0U;
			g_headset_status.mic_usb_level_bytes = 0U;
			k_sleep(K_MSEC(HEADSET_LOOP_SLEEP_MS));
			continue;
		}

		/* 1. PFSK RX → speaker FIFO */
		while (1) {
			struct pfsk_packet packet;
			uint8_t payload_bytes;

			if (!pfsk_session_rx_dequeue(&packet, K_NO_WAIT)) {
				break;
			}

			payload_bytes = packet.length - PFSK_PACKET_METADATA_LEN;
			if (payload_bytes != AUDIO_PFSK_SPK_PACKET_BYTES) {
				g_headset_status.overflow_bytes += packet.length;
				continue;
			}

			uint16_t remaining = tu_fifo_remaining(&g_headset_spk_fifo);
			if (remaining < payload_bytes) {
				g_headset_status.spk_dropped_oldest_bytes += payload_bytes - remaining;
			}

			(void)tu_fifo_write_n(&g_headset_spk_fifo, packet.payload, payload_bytes);
		}

		/* 2. I2S RX → PFSK TX (if session in service) */
		if (pfsk_session_get_state() == PFSK_SESSION_STATE_IN_SERVICE) {
			while (1) {
				uint8_t stereo[AUDIO_PFSK_MIC_PACKET_BYTES * 2U];
				struct pfsk_packet packet;
				uint8_t mono_bytes;

				if (tu_fifo_count(&g_headset_mic_fifo) < sizeof(stereo)) {
					break;
				}

				uint16_t read = tu_fifo_read_n(&g_headset_mic_fifo, stereo, sizeof(stereo));
				if (read < sizeof(stereo)) {
					break;
				}

				memset(&packet, 0, sizeof(packet));
				mono_bytes = audio_extract_left_to_mono(stereo, sizeof(stereo), packet.payload);
				if (mono_bytes != AUDIO_PFSK_MIC_PACKET_BYTES) {
					g_headset_status.overflow_bytes += sizeof(stereo);
					continue;
				}

				packet.length = PFSK_PACKET_METADATA_LEN + AUDIO_PFSK_MIC_PACKET_BYTES;

				if (!pfsk_session_tx_enqueue(&packet, K_NO_WAIT)) {
					if (pfsk_session_get_state() == PFSK_SESSION_STATE_IN_SERVICE) {
						g_headset_status.overflow_bytes += AUDIO_PFSK_MIC_PACKET_BYTES;
					}
				}
			}
		}

		/* 3. State machine & FIFO management */
		uint32_t fifo_bytes = tu_fifo_count(&g_headset_spk_fifo);
		uint32_t pending = audio_i2s_tx_get_pending_bytes();
		uint32_t level = fifo_bytes + pending;

		g_headset_status.spk_level_bytes = level;
		enum audio_path_state prev = g_headset_status.stream_state;
		g_headset_status.stream_state = audio_state_advance( g_headset_status.stream_state, level);

		if (prev != g_headset_status.stream_state) {
			printk("headset: %s\n", g_headset_status.stream_state == AUDIO_PATH_STATE_PLAYING ? "PLAYING" : "BUFFERING");

			if (g_headset_status.stream_state == AUDIO_PATH_STATE_PLAYING) {
				audio_i2s_activate(&g_headset_spk_fifo, &g_headset_mic_fifo);
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
