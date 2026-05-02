#include "audio_io/audio_path_wireless_headset.h"

#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(audio_path_pfsk_headset, LOG_LEVEL_INF);

#include "audio_io/audio_path_common.h"
#include "audio_io/i2s.h"
#include "audio_io/nau88l21.h"
#include "prop_fsk/session.h"

#define HEADSET_THREAD_STACK_SIZE  3072
#define HEADSET_THREAD_PRIORITY    6
#define HEADSET_LOOP_SLEEP_MS      1

#define HEADSET_AUDIO_TARGET_BYTES           960U
#define HEADSET_AUDIO_START_BYTES            960U
#define HEADSET_WARN_LOW_BYTES  (HEADSET_AUDIO_TARGET_BYTES * 10U / 50U )
#define HEADSET_WARN_HIGH_BYTES (HEADSET_AUDIO_TARGET_BYTES * 90U / 50U )

#define HEADSET_SPK_FIFO_SIZE      4096
#define HEADSET_MIC_FIFO_SIZE      4096

static struct audio_path_wireless_headset_status g_headset_status;

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
	g_headset_status = (struct audio_path_wireless_headset_status){
		.stream_state = AUDIO_PATH_STATE_BUFFERING,
		.spk_fll_target_rate_hz = (int32_t)AUDIO_I2S_SAMPLE_RATE_HZ,
	};
	audio_fll_set_auto();

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

void audio_path_wireless_headset_get_status(struct audio_path_wireless_headset_status *out)
{
	if (out != NULL) {
		*out = g_headset_status;
		if (g_audio_fll.fixed) {
			out->spk_fll_target_rate_hz = g_audio_fll.fixed_rate_hz;
		}
	}
}


static void headset_update_codec_clock(uint32_t fifo_lvl, uint32_t pending)
{
	if (g_audio_fll.fixed) {
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
		HEADSET_AUDIO_TARGET_BYTES, &filter, &i_sum,
		AUDIO_P_GAIN, AUDIO_P_KI,
		fifo_lvl, pending);

	g_headset_status.spk_filtered_level_bytes = (uint32_t)filter;
	g_headset_status.spk_error_bytes = (int32_t)HEADSET_AUDIO_TARGET_BYTES - (int32_t)g_headset_status.spk_filtered_level_bytes;
	g_headset_status.spk_p_adjust_hz = adjust_hz;

	int32_t target_rate = (int32_t)AUDIO_I2S_SAMPLE_RATE_HZ - adjust_hz;
	int ret = nau88l21_set_fll_target_rate_hz(target_rate);

	if (ret == 0) {
		g_headset_status.spk_fll_target_rate_hz = target_rate;
	} else {
		LOG_ERR("Failed to set codec FLL target rate to %d Hz", target_rate);
	}
}

static void headset_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	tu_fifo_t *spk_ff = &g_headset_spk_fifo;
	tu_fifo_t *mic_ff = &g_headset_mic_fifo;

	while (1) {
		uint32_t spk_ff_bytes = tu_fifo_count(spk_ff);
		uint32_t pending = audio_i2s_tx_get_pending_bytes();
		uint32_t level = spk_ff_bytes + pending;

		g_headset_status.spk_fifo_bytes = spk_ff_bytes;
		g_headset_status.spk_pending_bytes = pending;

		if (g_headset_status.stream_state == AUDIO_PATH_STATE_BUFFERING) {
			if (level >= HEADSET_AUDIO_START_BYTES) {
				g_headset_status.stream_state = AUDIO_PATH_STATE_PLAYING;
				LOG_INF("switching to PLAYING, notifying i2s thread...");
				audio_i2s_activate(spk_ff, mic_ff);
			}
		} else if (pending == 0U && spk_ff_bytes < AUDIO_I2S_BLOCK_BYTES) {
			g_headset_status.stream_state = AUDIO_PATH_STATE_BUFFERING;
			LOG_INF("switching to BUFFERING, notifying i2s thread...");
			audio_i2s_deactivate();
			g_headset_status.spk_underrun_events++;
		}

		/* PFSK RX → Speaker FIFO */
		while (1) {
			struct pfsk_packet packet;
			uint8_t payload_bytes;

			uint16_t spk_ff_remaining = tu_fifo_remaining(spk_ff);
			if(spk_ff_remaining < AUDIO_PFSK_SPK_PACKET_BYTES) {
				break;
			}

			if (!pfsk_session_rx_dequeue(&packet, K_NO_WAIT)) {
				break;
			}

			payload_bytes = packet.length - PFSK_PACKET_METADATA_LEN;
			if (payload_bytes != AUDIO_PFSK_SPK_PACKET_BYTES) {
				LOG_ERR("Invalid payload size! Expected %d bytes, got %d bytes", AUDIO_PFSK_SPK_PACKET_BYTES, payload_bytes);
				break;
			}

			uint16_t written = tu_fifo_write_n(spk_ff, packet.payload, payload_bytes);
			if (written != payload_bytes) {
				LOG_ERR("Invalid bytes written to speaker FIFO! Expected %d bytes, wrote %d bytes", payload_bytes, written);
				break;
			}
		}

		/* Microphone FIFO → PFSK TX */
		while(1) {
			struct pfsk_packet packet;
			uint32_t payload_bytes;

			uint16_t mic_ff_count = tu_fifo_count(mic_ff);
			if(mic_ff_count < AUDIO_PFSK_MIC_PACKET_BYTES) {
				break;
			}
			
			memset(&packet, 0, sizeof(packet));
			payload_bytes = tu_fifo_read_n(mic_ff, packet.payload, AUDIO_PFSK_MIC_PACKET_BYTES);

			if (payload_bytes != AUDIO_PFSK_MIC_PACKET_BYTES) {
				LOG_ERR("Invalid payload size from microphone FIFO! Expected %d bytes, got %d bytes", AUDIO_PFSK_MIC_PACKET_BYTES, payload_bytes);
				break;
			}

			packet.length = PFSK_PACKET_METADATA_LEN + AUDIO_PFSK_MIC_PACKET_BYTES;

			if (!pfsk_session_tx_enqueue(&packet, K_NO_WAIT)) {
				LOG_WRN_RATELIMIT_RATE(10000, "Failed to enqueue PFSK packet for transmission!");
				break;
			}
		}

		/* FLL controller */
		if (g_headset_status.stream_state == AUDIO_PATH_STATE_PLAYING) {
			headset_update_codec_clock(spk_ff_bytes, pending);
			#ifdef WARN_SPK_LVL
			warn_on_level(level, spk_ff_bytes, pending, HEADSET_WARN_LOW_BYTES, HEADSET_WARN_HIGH_BYTES);
			#endif
		}

		k_sleep(K_MSEC(HEADSET_LOOP_SLEEP_MS));
	}
}
