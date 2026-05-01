#include "audio_io/audio_path_wireless_dongle.h"

#include <string.h>

#include <zephyr/kernel.h>

#include "tusb.h"

#include "audio_io/audio_path_common.h"
#include "prop_gfsk/link.h"
#include "prop_gfsk/test_mode.h"
#include "usb/usb_audio.h"

#define DONGLE_THREAD_STACK_SIZE  3072
#define DONGLE_THREAD_PRIORITY    6
#define DONGLE_LOOP_SLEEP_MS      1

static struct audio_path_wireless_status g_dongle_status;

static K_THREAD_STACK_DEFINE(g_dongle_thread_stack, DONGLE_THREAD_STACK_SIZE);
static struct k_thread g_dongle_thread;

static void dongle_thread(void *a, void *b, void *c);

void audio_path_wireless_dongle_init(void)
{
	g_dongle_status.stream_state = AUDIO_PATH_STATE_BUFFERING;

	k_thread_create(&g_dongle_thread, g_dongle_thread_stack,
			K_THREAD_STACK_SIZEOF(g_dongle_thread_stack),
			dongle_thread, NULL, NULL, NULL,
			DONGLE_THREAD_PRIORITY, 0, K_NO_WAIT);
}

void audio_path_wireless_dongle_get_status(struct audio_path_wireless_status *out)
{
	if (out != NULL) {
		*out = g_dongle_status;
	}
}

static void dongle_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (1) {
		if (pgfsk_test_mode_is_running()) {
			g_dongle_status.stream_state = AUDIO_PATH_STATE_BUFFERING;
			g_dongle_status.spk_level_bytes = 0U;
		} else {
			/* 1. PGFSK RX → USB mic EP IN */
			while (1) {
				struct pgfsk_frame frame;
				size_t pushed;

				if (!pgfsk_link_rx_dequeue(&frame, K_NO_WAIT)) {
					break;
				}

				pushed = usb_audio_write_microphone_bytes(
					frame.payload, frame.len);
				if (pushed < frame.len) {
					g_dongle_status.overflow_bytes +=
						frame.len - pushed;
				}
			}

			/* 2. Stream state from USB EP OUT FIFO level */
			{
				tu_fifo_t *usb_ff = tud_audio_get_ep_out_ff();
				uint32_t level = (usb_ff != NULL) ? tu_fifo_count(usb_ff) : 0U;

				g_dongle_status.spk_level_bytes = level;
				g_dongle_status.spk_usb_level_bytes = level;
				g_dongle_status.stream_state =
					audio_state_advance(
						g_dongle_status.stream_state,
						level);
			}

			/* 3. If PLAYING: USB EP OUT FIFO → PGFSK TX */
			if (g_dongle_status.stream_state == AUDIO_PATH_STATE_PLAYING) {
				tu_fifo_t *usb_ff = tud_audio_get_ep_out_ff();

				if (pgfsk_link_get_state() == PGFSK_LINK_STATE_IN_SERVICE && usb_ff != NULL) {
					while (tu_fifo_count(usb_ff) >= AUDIO_STEP_BYTES) {
						struct pgfsk_frame frame;
						uint32_t got;

						memset(&frame, 0, sizeof(frame));
						got = tu_fifo_read_n( usb_ff, frame.payload, AUDIO_STEP_BYTES);

						if (got != AUDIO_STEP_BYTES) {
							break;
						}

						frame.len = got;

						if (!pgfsk_link_tx_enqueue(&frame, K_NO_WAIT)) {
							g_dongle_status.overflow_bytes += got;
							break;
						}
					}
				}
			}

			/* 4. Mic USB level telemetry */
			g_dongle_status.mic_usb_level_bytes = usb_audio_microphone_level_bytes();
		}

		k_sleep(K_MSEC(DONGLE_LOOP_SLEEP_MS));
	}
}
