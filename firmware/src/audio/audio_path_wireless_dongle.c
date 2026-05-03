#include "audio/audio_path_wireless_dongle.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(audio_path_pfsk_dongle, LOG_LEVEL_INF);
#include "tusb.h"
#include "prop_fsk/session.h"

#define DONGLE_THREAD_STACK_SIZE  3072
#define DONGLE_THREAD_PRIORITY    6
#define DONGLE_LOOP_SLEEP_MS      1

static struct audio_path_wireless_dongle_status dongle_status;

static K_THREAD_STACK_DEFINE(dongle_thread_stack, DONGLE_THREAD_STACK_SIZE);
static struct k_thread dongle_thread_data;

static void dongle_thread(void *a, void *b, void *c);

void audio_path_wireless_dongle_init(void)
{
	k_thread_create(&dongle_thread_data, dongle_thread_stack,
			K_THREAD_STACK_SIZEOF(dongle_thread_stack),
			dongle_thread, NULL, NULL, NULL,
			DONGLE_THREAD_PRIORITY, 0, K_NO_WAIT);
}

void audio_path_wireless_dongle_get_status(struct audio_path_wireless_dongle_status *out)
{
	if (out != NULL) {
		*out = dongle_status;
	}
}

static void dongle_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	tu_fifo_t *ep_out_ff = NULL;
	tu_fifo_t *ep_in_ff = NULL;

	while (ep_out_ff == NULL || ep_in_ff == NULL) {
		ep_out_ff = tud_audio_get_ep_out_ff();
		ep_in_ff = tud_audio_get_ep_in_ff();
		k_sleep(K_MSEC(DONGLE_LOOP_SLEEP_MS));
	}

	while (1) {
		/* PFSK RX → USB mic EP IN */
		while (1) {
			struct pfsk_packet packet;
			uint8_t payload_bytes;

			uint16_t ep_in_remaining = tu_fifo_remaining(ep_in_ff);
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

			uint16_t written = tu_fifo_write_n(ep_in_ff, packet.payload, payload_bytes);
			if (written != payload_bytes) {
				LOG_ERR("Invalid bytes written to USB EP IN FIFO! Expected %d bytes, wrote %d bytes", payload_bytes, written);
				break;
			}
		}

		/* USB EP OUT FIFO → PFSK TX */
		while(1) {
			struct pfsk_packet packet;
			uint32_t payload_bytes;

			uint16_t ep_out_count = tu_fifo_count(ep_out_ff);
			if(ep_out_count < AUDIO_PFSK_SPK_PACKET_BYTES) {
				break;
			}
			
			memset(&packet, 0, sizeof(packet));
			payload_bytes = tu_fifo_read_n(ep_out_ff, packet.payload, AUDIO_PFSK_SPK_PACKET_BYTES);

			if (payload_bytes != AUDIO_PFSK_SPK_PACKET_BYTES) {
				LOG_ERR("Invalid payload size from USB EP OUT! Expected %d bytes, got %d bytes", AUDIO_PFSK_SPK_PACKET_BYTES, payload_bytes);
				break;
			}

			packet.length = PFSK_PACKET_METADATA_LEN + AUDIO_PFSK_SPK_PACKET_BYTES;

			if (!pfsk_session_tx_enqueue(&packet, K_NO_WAIT)) {
				dongle_status.overflow_bytes += payload_bytes;
				break;
			}
		}

		k_sleep(K_MSEC(DONGLE_LOOP_SLEEP_MS));
	}
}
