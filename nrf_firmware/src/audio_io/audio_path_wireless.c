#include "audio_io/audio_path_wireless.h"

#include <string.h>

#include <zephyr/kernel.h>

#include "app_control.h"
#include "audio_io/audio_path_common.h"
#include "audio_io/i2s.h"
#include "prop_gfsk/link.h"
#include "prop_gfsk/test_mode.h"
#include "tusb.h"
#include "usb/usb_audio.h"

#define WIRELESS_HEADER_BYTES       1U

#define WIRELESS_THREAD_STACK_SIZE  3072
#define WIRELESS_THREAD_PRIORITY    6
#define WIRELESS_RING_BYTES         4096
#define WIRELESS_LOOP_SLEEP_MS      1

BUILD_ASSERT(AUDIO_P_ADJUST_MAX_HZ <= INT8_MAX,
	     "wireless feedback metadata stores adjust_hz in int8_t");

static uint8_t g_wireless_ring_buf[WIRELESS_RING_BYTES] __aligned(4);
static tu_fifo_t g_wireless_ring_ff;
static float g_wireless_feedback_filter = -1.0f;
static int8_t g_peer_adjust_hz;
/* Wakes the parked wireless worker on (re)activation. */
static K_SEM_DEFINE(g_wireless_start_sem, 0, 1);
/* Acknowledges deactivate() after worker teardown. */
static K_SEM_DEFINE(g_wireless_stop_sem, 0, 1);
static struct audio_path_wireless_status g_wireless_status;

static inline void wireless_encode_peer_meta(uint8_t *hdr, int8_t adjust_hz)
{
	hdr[0] = (uint8_t)adjust_hz;
}

static inline int8_t wireless_decode_peer_adjust_hz(const uint8_t *hdr)
{
	return (int8_t)hdr[0];
}

static void wireless_update_local_adjust(void)
{
	uint32_t raw = tu_fifo_count(&g_wireless_ring_ff);
	uint32_t filtered = audio_filter_update(&g_wireless_feedback_filter, raw);
	int32_t error_bytes = (int32_t)AUDIO_TARGET_BYTES - (int32_t)filtered;
	int32_t adjust_hz = audio_p_controller_step(error_bytes, filtered);

	g_wireless_status.spk_p_adjust_hz = adjust_hz;
}

static bool wireless_parse_audio_frame(const struct pgfsk_frame *frame,
				       int8_t *peer_adjust_hz_out,
				       const uint8_t **payload_out,
				       size_t *payload_bytes_out)
{
	if (frame->len < WIRELESS_HEADER_BYTES) {
		return false;
	}

	*peer_adjust_hz_out = wireless_decode_peer_adjust_hz(&frame->payload[0]);
	*payload_out = &frame->payload[WIRELESS_HEADER_BYTES];
	*payload_bytes_out = frame->len - WIRELESS_HEADER_BYTES;
	return true;
}

static void wireless_reset_state(void)
{
	tu_fifo_clear(&g_wireless_ring_ff);
	g_wireless_feedback_filter = -1.0f;
	g_peer_adjust_hz = 0;
	memset(&g_wireless_status, 0, sizeof(g_wireless_status));
	g_wireless_status.stream_state = AUDIO_PATH_STATE_BUFFERING;
	usb_audio_reset();
	audio_i2s_tx_flush();
	audio_i2s_rx_flush();
}

static void wireless_ingest_usb_speaker(void)
{
	uint8_t tmp[AUDIO_DMA_MAX_BYTES];

	while (1) {
		uint16_t got = tud_audio_read(tmp, (uint16_t)sizeof(tmp));

		if (got == 0U) {
			break;
		}

		uint32_t evicted = audio_ring_push(&g_wireless_ring_ff, tmp, got);

		if (evicted > 0U) {
			g_wireless_status.spk_dropped_oldest_bytes += evicted;
			if (pgfsk_link_get_state() == PGFSK_LINK_STATE_IN_SERVICE) {
				g_wireless_status.overflow_bytes += evicted;
			}
		}
	}
}

static void wireless_ingest_pgfsk_playback(void)
{
	while (1) {
		struct pgfsk_frame frame;
		const uint8_t *payload;
		size_t payload_bytes;
		int8_t peer_adjust_hz;
		uint32_t evicted;

		if (!pgfsk_link_rx_dequeue(&frame, K_NO_WAIT)) {
			break;
		}

		if (!wireless_parse_audio_frame(&frame, &peer_adjust_hz,
						&payload, &payload_bytes)) {
			g_wireless_status.rx_malformed_frames++;
			continue;
		}

		evicted = audio_ring_push(&g_wireless_ring_ff, payload,
					  (uint32_t)payload_bytes);

		if (evicted > 0U) {
			g_wireless_status.spk_dropped_oldest_bytes += evicted;
			g_wireless_status.overflow_bytes += evicted;
		}
	}
}

static void wireless_send_pgfsk_capture_to_usb(void)
{
	while (1) {
		struct pgfsk_frame frame;
		const uint8_t *payload;
		size_t payload_bytes;
		int8_t peer_adjust_hz;
		size_t pushed;

		if (!pgfsk_link_rx_dequeue(&frame, K_NO_WAIT)) {
			break;
		}

		if (!wireless_parse_audio_frame(&frame, &peer_adjust_hz,
						&payload, &payload_bytes)) {
			g_wireless_status.rx_malformed_frames++;
			continue;
		}

		g_peer_adjust_hz = peer_adjust_hz;
		g_wireless_status.peer_adjust_hz = peer_adjust_hz;

		pushed = usb_audio_write_microphone_bytes(payload, payload_bytes);
		if (pushed < payload_bytes) {
			g_wireless_status.overflow_bytes += payload_bytes - pushed;
		}
	}
}

static void wireless_send_i2s_capture_to_pgfsk(void)
{
	while (1) {
		struct audio_i2s_block block;
		struct pgfsk_frame frame;
		size_t mono_bytes;
		size_t max_payload = PGFSK_PAYLOAD_MAX_LEN - WIRELESS_HEADER_BYTES;

		if (audio_i2s_rx_dequeue_block(&block, K_NO_WAIT) != 0) {
			break;
		}

		memset(&frame, 0, sizeof(frame));
		mono_bytes = audio_extract_left_to_mono(
			block.bytes,
			AUDIO_I2S_BLOCK_BYTES,
			&frame.payload[WIRELESS_HEADER_BYTES],
			max_payload);
		if (mono_bytes == 0U) {
			g_wireless_status.overflow_bytes += AUDIO_I2S_BLOCK_BYTES;
			continue;
		}

		wireless_encode_peer_meta(&frame.payload[0],
					  (int8_t)g_wireless_status.spk_p_adjust_hz);
		frame.len = WIRELESS_HEADER_BYTES + mono_bytes;

		if (!pgfsk_link_tx_enqueue(&frame, K_NO_WAIT)) {
			if (pgfsk_link_get_state() == PGFSK_LINK_STATE_IN_SERVICE) {
				g_wireless_status.overflow_bytes += mono_bytes;
			}
		}
	}
}

static void wireless_send_playback_to_pgfsk(void)
{
	BUILD_ASSERT(AUDIO_STEP_BYTES + WIRELESS_HEADER_BYTES <= PGFSK_PAYLOAD_MAX_LEN);

	if (pgfsk_link_get_state() != PGFSK_LINK_STATE_IN_SERVICE) {
		return;
	}

	while (tu_fifo_count(&g_wireless_ring_ff) >= AUDIO_STEP_BYTES) {
		struct pgfsk_frame frame;
		uint32_t got;

		memset(&frame, 0, sizeof(frame));
		got = tu_fifo_read_n(&g_wireless_ring_ff,
				     &frame.payload[WIRELESS_HEADER_BYTES],
				     AUDIO_STEP_BYTES);

		if (got != AUDIO_STEP_BYTES) {
			break;
		}

		wireless_encode_peer_meta(&frame.payload[0], 0U);
		frame.len = WIRELESS_HEADER_BYTES + got;

		if (!pgfsk_link_tx_enqueue(&frame, K_NO_WAIT)) {
			if (pgfsk_link_get_state() == PGFSK_LINK_STATE_IN_SERVICE) {
				g_wireless_status.overflow_bytes += got;
			}
			break;
		}
	}
}

static void wireless_send_playback_to_i2s(void)
{
	if (g_wireless_status.stream_state != AUDIO_PATH_STATE_PLAYING) {
		return;
	}

	while (audio_i2s_tx_enqueue_fifo(&g_wireless_ring_ff) == 0) { }

	if (g_wireless_status.spk_level_bytes <= AUDIO_PANIC_LOW_BYTES) {
		struct audio_i2s_block silence = { 0 };

		if (audio_i2s_tx_enqueue_block(&silence, K_NO_WAIT) == 0) {
			g_wireless_status.spk_underrun_bytes += AUDIO_I2S_BLOCK_BYTES;
			g_wireless_status.spk_silence_inserted_bytes += AUDIO_I2S_BLOCK_BYTES;
		}
	}
}

static void wireless_update_feedback_dongle(void)
{
	int8_t adjust_hz = 0;
	int32_t rate_hz;

	if (pgfsk_link_get_state() == PGFSK_LINK_STATE_IN_SERVICE) {
		adjust_hz = g_peer_adjust_hz;
	}

	rate_hz = (int32_t)AUDIO_I2S_SAMPLE_RATE_HZ + (int32_t)adjust_hz;
	if (rate_hz < 1000) {
		rate_hz = 1000;
	}

	g_wireless_status.peer_adjust_hz = adjust_hz;
	(void)tud_audio_fb_set((uint32_t)(((uint64_t)rate_hz << 16) / 8000ULL));
}

static void wireless_update_stream_state(void)
{
	uint32_t level = tu_fifo_count(&g_wireless_ring_ff);

	g_wireless_status.spk_level_bytes = level;
	g_wireless_status.stream_state = audio_state_advance(g_wireless_status.stream_state, level);
}

static void wireless_update_usb_levels(void)
{
	if (app_control_get_current_profile() != APP_PROFILE_PGFSK_DONGLE) {
		g_wireless_status.spk_usb_level_bytes = 0U;
		g_wireless_status.mic_usb_level_bytes = 0U;
		return;
	}

	g_wireless_status.spk_usb_level_bytes = tud_audio_available();
	g_wireless_status.mic_usb_level_bytes = usb_audio_microphone_level_bytes();
}

static void wireless_step_test_mode(void)
{
	tu_fifo_clear(&g_wireless_ring_ff);
	g_wireless_status.stream_state = AUDIO_PATH_STATE_BUFFERING;
	g_wireless_status.spk_level_bytes = 0U;
	g_wireless_status.peer_adjust_hz = 0;
	g_wireless_status.spk_p_adjust_hz = 0;
	g_wireless_feedback_filter = -1.0f;
	g_peer_adjust_hz = 0;
	wireless_update_usb_levels();
}

static void wireless_step_dongle(void)
{
	wireless_ingest_usb_speaker();
	wireless_send_pgfsk_capture_to_usb();

	wireless_update_stream_state();
	if (g_wireless_status.stream_state == AUDIO_PATH_STATE_PLAYING) {
		wireless_send_playback_to_pgfsk();
	}
	wireless_update_feedback_dongle();
	wireless_update_usb_levels();
}

static void wireless_step_headset(void)
{
	wireless_ingest_pgfsk_playback();
	wireless_update_local_adjust();
	if (pgfsk_link_get_state() == PGFSK_LINK_STATE_IN_SERVICE) {
		wireless_send_i2s_capture_to_pgfsk();
	} else {
		audio_i2s_rx_flush();
	}

	wireless_update_stream_state();
	wireless_send_playback_to_i2s();
	wireless_update_usb_levels();
}

static void wireless_step(void)
{
	if (pgfsk_test_mode_is_running()) {
		wireless_step_test_mode();
		return;
	}

	if (app_control_get_current_profile() == APP_PROFILE_PGFSK_DONGLE) {
		wireless_step_dongle();
		return;
	}

	wireless_step_headset();
}

void audio_path_wireless_activate(void)
{
	if (g_wireless_status.active) {
		return;
	}

	wireless_reset_state();
	g_wireless_status.active = true;
	k_sem_give(&g_wireless_start_sem);
}

void audio_path_wireless_deactivate(void)
{
	if (!g_wireless_status.active) {
		return;
	}

	g_wireless_status.active = false;
	(void)k_sem_take(&g_wireless_stop_sem, K_FOREVER);
}

void audio_path_wireless_get_status(struct audio_path_wireless_status *out)
{
	if (out == NULL) {
		return;
	}

	*out = g_wireless_status;
}

static void wireless_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	tu_fifo_config(&g_wireless_ring_ff, g_wireless_ring_buf,
		       sizeof(g_wireless_ring_buf), true);

	while (1) {
		/* Park until activate() selects the wireless path. */
		(void)k_sem_take(&g_wireless_start_sem, K_FOREVER);

		while (g_wireless_status.active) {
			wireless_step();
			k_sleep(K_MSEC(WIRELESS_LOOP_SLEEP_MS));
		}

		/* Tell deactivate() that the worker has exited. */
		k_sem_give(&g_wireless_stop_sem);
	}
}

K_THREAD_DEFINE(wireless_thread_id, WIRELESS_THREAD_STACK_SIZE, wireless_thread, NULL, NULL, NULL, WIRELESS_THREAD_PRIORITY, 0, 0);
