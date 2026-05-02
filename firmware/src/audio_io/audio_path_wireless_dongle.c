#include "audio_io/audio_path_wireless_dongle.h"

#include <string.h>

#include <zephyr/kernel.h>

#include "tusb.h"

#include "prop_fsk/session.h"
#include "usb/usb_audio.h"
#include "zephyr/logging/log.h"
LOG_MODULE_REGISTER(audio_path_wireless_dongle, LOG_LEVEL_INF);

#define DONGLE_THREAD_STACK_SIZE  3072
#define DONGLE_THREAD_PRIORITY    6
#define DONGLE_LOOP_SLEEP_MS      1

static struct audio_path_wireless_dongle_status g_dongle_status;

static K_THREAD_STACK_DEFINE(g_dongle_thread_stack, DONGLE_THREAD_STACK_SIZE);
static struct k_thread g_dongle_thread;

static void dongle_thread(void *a, void *b, void *c);

void audio_path_wireless_dongle_init(void)
{
	k_thread_create(&g_dongle_thread, g_dongle_thread_stack,
			K_THREAD_STACK_SIZEOF(g_dongle_thread_stack),
			dongle_thread, NULL, NULL, NULL,
			DONGLE_THREAD_PRIORITY, 0, K_NO_WAIT);
}

void audio_path_wireless_dongle_get_status(struct audio_path_wireless_dongle_status *out)
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

	while (tud_audio_get_ep_out_ff() == NULL || tud_audio_get_ep_in_ff() == NULL) {
		k_sleep(K_MSEC(DONGLE_LOOP_SLEEP_MS));
	}

	while (1) {
		/* PFSK RX → USB mic EP IN */
		while (1) {
			struct pfsk_packet packet;
			uint8_t payload_bytes;

			uint16_t ep_in_remaining = tu_fifo_remaining(tud_audio_get_ep_in_ff());
			if(ep_in_remaining < AUDIO_PFSK_MIC_PACKET_BYTES) {
				break;
			}

			if (!pfsk_session_rx_dequeue(&packet, K_NO_WAIT)) {
				break;
			}

			payload_bytes = packet.length - PFSK_PACKET_METADATA_LEN;
			if (payload_bytes != AUDIO_PFSK_MIC_PACKET_BYTES) {
				LOG_ERR("Invalid payload size! Expected %d bytes, got %d bytes", AUDIO_PFSK_MIC_PACKET_BYTES, payload_bytes);
				break;
			}

			uint16_t written = tu_fifo_write_n(tud_audio_get_ep_in_ff(), packet.payload, payload_bytes);
			if (written != payload_bytes) {
				LOG_ERR("Invalid bytes written to USB EP IN FIFO! Expected %d bytes, wrote %d bytes", payload_bytes, written);
				break;
			}
		}

		/* USB EP OUT FIFO → PFSK TX */
		while(1) {
			struct pfsk_packet packet;
			uint32_t payload_bytes;

			uint16_t ep_out_count = tu_fifo_count(tud_audio_get_ep_out_ff());
			if(ep_out_count < AUDIO_PFSK_SPK_PACKET_BYTES) {
				break;
			}
			
			memset(&packet, 0, sizeof(packet));
			payload_bytes = tu_fifo_read_n(tud_audio_get_ep_out_ff(), packet.payload, AUDIO_PFSK_SPK_PACKET_BYTES);

			if (payload_bytes != AUDIO_PFSK_SPK_PACKET_BYTES) {
				LOG_ERR("Invalid payload size from USB EP OUT! Expected %d bytes, got %d bytes", AUDIO_PFSK_SPK_PACKET_BYTES, payload_bytes);
				break;
			}

			packet.length = PFSK_PACKET_METADATA_LEN + AUDIO_PFSK_SPK_PACKET_BYTES;

			if (!pfsk_session_tx_enqueue(&packet, K_NO_WAIT)) {
				g_dongle_status.overflow_bytes += payload_bytes;
				break;
			}
		}

		k_sleep(K_MSEC(DONGLE_LOOP_SLEEP_MS));
	}
}
