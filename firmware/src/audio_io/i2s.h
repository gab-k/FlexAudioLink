#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#include "tusb.h"

#define AUDIO_I2S_SAMPLE_RATE_HZ           	48000U
#define AUDIO_I2S_CHANNELS                 	2U
#define AUDIO_I2S_WORD_SIZE_BITS           	16U
#define AUDIO_I2S_BYTES_PER_CHANNEL_SAMPLE 	2U
#define AUDIO_I2S_BYTES_PER_STEREO_SAMPLE  	(AUDIO_I2S_CHANNELS * AUDIO_I2S_BYTES_PER_CHANNEL_SAMPLE)
#define AUDIO_I2S_BYTES_PER_MS 				((AUDIO_I2S_SAMPLE_RATE_HZ / 1000U) * AUDIO_I2S_BYTES_PER_STEREO_SAMPLE)
#define AUDIO_I2S_BLOCK_BYTES              	(AUDIO_I2S_BYTES_PER_MS / 2U)
#define AUDIO_I2S_STEREO_SAMPLES_PER_BLOCK  (AUDIO_I2S_BLOCK_BYTES / AUDIO_I2S_BYTES_PER_STEREO_SAMPLE)
#define AUDIO_I2S_PCM16_SAMPLES_PER_BLOCK   (AUDIO_I2S_BLOCK_BYTES / sizeof(int16_t))

struct audio_i2s_block {
	union {
		uint8_t bytes[AUDIO_I2S_BLOCK_BYTES];
		int16_t pcm16[AUDIO_I2S_PCM16_SAMPLES_PER_BLOCK];
	};
};

/* Start I2S playback/mic capture. Blocks until TDM is running.
 * tx_fifo: speaker data source (NULL disallowed).
 * rx_fifo: mic data sink (NULL to discard mic). */
void audio_i2s_activate(tu_fifo_t *tx_fifo, tu_fifo_t *rx_fifo);

/* Stop I2S. Blocks until TDM is stopped and reconfigured. */
void audio_i2s_deactivate(void);

uint32_t audio_i2s_tx_get_pending_bytes(void);
