#include "audio/path_headset.h"

#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(path_headset, LOG_LEVEL_INF);

#include "audio/path_common.h"
#include "audio/i2s.h"
#include "prop/session.h"

#define HEADSET_THREAD_STACK_SIZE  3072
#define HEADSET_THREAD_PRIORITY    6
#define HEADSET_LOOP_SLEEP_MS      1

#define HEADSET_TARGET_BYTES  	(HEADSET_SPK_FIFO_SIZE/2)
#define HEADSET_START_BYTES  	(HEADSET_TARGET_BYTES)
#define HEADSET_WARN_LOW_BYTES  (HEADSET_SPK_FIFO_SIZE * 10U / 100U )
#define HEADSET_WARN_HIGH_BYTES (HEADSET_SPK_FIFO_SIZE * 90U / 100U )

#define HEADSET_SPK_FIFO_SIZE      1920U
#define HEADSET_MIC_FIFO_SIZE      960U

static struct codec_path_status status;

static uint8_t headset_spk_fifo_buf[HEADSET_SPK_FIFO_SIZE];
static uint8_t headset_mic_fifo_buf[HEADSET_MIC_FIFO_SIZE];
static tu_fifo_t headset_spk_fifo;
static tu_fifo_t headset_mic_fifo;
static OSAL_MUTEX_DEF(headset_spk_mutex_wr);
static OSAL_MUTEX_DEF(headset_spk_mutex_rd);
static OSAL_MUTEX_DEF(headset_mic_mutex_wr);
static OSAL_MUTEX_DEF(headset_mic_mutex_rd);

static K_THREAD_STACK_DEFINE(headset_thread_stack, HEADSET_THREAD_STACK_SIZE);
static struct k_thread headset_thread_data;

static void headset_thread(void *a, void *b, void *c);

void path_headset_init(void)
{
	status = (struct codec_path_status){
		.stream_state = PATH_STATE_BUFFERING,
		.spk_fll_target_rate_hz = (int32_t)AUDIO_I2S_SAMPLE_RATE_HZ,
	};
	fll_set_auto();

	tu_fifo_config(&headset_spk_fifo, headset_spk_fifo_buf,
		       HEADSET_SPK_FIFO_SIZE, true);
	tu_fifo_config_mutex(&headset_spk_fifo,
			     osal_mutex_create(&headset_spk_mutex_wr),
			     osal_mutex_create(&headset_spk_mutex_rd));
	tu_fifo_config(&headset_mic_fifo, headset_mic_fifo_buf,
		       HEADSET_MIC_FIFO_SIZE, true);
	tu_fifo_config_mutex(&headset_mic_fifo,
			     osal_mutex_create(&headset_mic_mutex_wr),
			     osal_mutex_create(&headset_mic_mutex_rd));

	k_thread_create(&headset_thread_data, headset_thread_stack,
			K_THREAD_STACK_SIZEOF(headset_thread_stack),
			headset_thread, NULL, NULL, NULL,
			HEADSET_THREAD_PRIORITY, 0, K_NO_WAIT);
}

void path_headset_get_status(struct codec_path_status *out)
{
	if (out != NULL) {
		*out = status;
		if (fll.fixed) {
			out->spk_fll_target_rate_hz = fll.fixed_rate_hz;
		}
	}
}


static void headset_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	tu_fifo_t *spk_ff = &headset_spk_fifo;
	tu_fifo_t *mic_ff = &headset_mic_fifo;
	uint32_t now_ms;

	while (1) {
		uint32_t spk_ff_bytes = tu_fifo_count(spk_ff);
		uint32_t pending = audio_i2s_tx_get_pending_bytes();
		uint32_t level = spk_ff_bytes + pending;

		status.spk_fifo_bytes = spk_ff_bytes;
		status.spk_pending_bytes = pending;

		if (status.stream_state == PATH_STATE_BUFFERING) {
			if (level >= HEADSET_START_BYTES) {
				status.stream_state = PATH_STATE_PLAYING;
				LOG_INF("switching to PLAYING, notifying i2s thread...");
				audio_i2s_activate(spk_ff, mic_ff);
			}
		} else if (pending == 0U && spk_ff_bytes < AUDIO_I2S_BLOCK_BYTES) {
			status.stream_state = PATH_STATE_BUFFERING;
			LOG_INF("switching to BUFFERING, notifying i2s thread...");
			audio_i2s_deactivate();
			status.spk_underrun_events++;
		}

		/* PROP RX → Speaker FIFO */
		while (1) {
			struct prop_packet packet;
			uint8_t payload_bytes;

			uint16_t spk_ff_remaining = tu_fifo_remaining(spk_ff);
			if(spk_ff_remaining < PROP_SPK_PACKET_BYTES) {
				break;
			}

			if (!prop_session_rx_dequeue(&packet, K_NO_WAIT)) {
				break;
			}

			payload_bytes = packet.length - PROP_PACKET_METADATA_LEN;
			if (payload_bytes != PROP_SPK_PACKET_BYTES) {
				LOG_ERR("Invalid payload size! Expected %d bytes, got %d bytes", PROP_SPK_PACKET_BYTES, payload_bytes);
				break;
			}

			uint16_t written = tu_fifo_write_n(spk_ff, packet.payload, payload_bytes);
			if (written != payload_bytes) {
				LOG_ERR("Invalid bytes written to speaker FIFO! Expected %d bytes, wrote %d bytes", payload_bytes, written);
				break;
			}
		}

		/* Microphone FIFO → PROP TX */
		while(1) {
			struct prop_packet packet;
			uint32_t payload_bytes;

			uint16_t mic_ff_count = tu_fifo_count(mic_ff);
			if(mic_ff_count < PROP_MIC_PACKET_BYTES) {
				break;
			}

			memset(&packet, 0, sizeof(packet));
			payload_bytes = tu_fifo_read_n(mic_ff, packet.payload, PROP_MIC_PACKET_BYTES);

			if (payload_bytes != PROP_MIC_PACKET_BYTES) {
				LOG_ERR("Invalid payload size from microphone FIFO! Expected %d bytes, got %d bytes", PROP_MIC_PACKET_BYTES, payload_bytes);
				break;
			}

			packet.length = PROP_PACKET_METADATA_LEN + PROP_MIC_PACKET_BYTES;

			if (!prop_session_tx_enqueue(&packet, K_NO_WAIT)) {
				LOG_WRN_RATELIMIT_RATE(10000, "Failed to enqueue PROP packet for transmission!");
				break;
			}
		}

		/* FLL controller */
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
				status.spk_error_bytes = (int32_t)HEADSET_TARGET_BYTES - (int32_t)status.spk_filtered_level_bytes;
				status.spk_p_adjust_hz = codec_clock_controller(status.spk_error_bytes,
										AUDIO_I2S_SAMPLE_RATE_HZ,
										&status.spk_fll_target_rate_hz);
				last_controller_ms = now_ms;
			}

			monitor_codec_level(&status, HEADSET_WARN_LOW_BYTES, HEADSET_WARN_HIGH_BYTES);
		}

		k_sleep(K_MSEC(HEADSET_LOOP_SLEEP_MS));
	}
}
