#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#include "audio/audio_config.h"
#include "tusb.h"

/* Start I2S playback/mic capture. Blocks until TDM is running.
 * tx_fifo: speaker data source (NULL disallowed).
 * rx_fifo: mic data sink (NULL to discard mic). */
void audio_i2s_activate(tu_fifo_t *tx_fifo, tu_fifo_t *rx_fifo);

/* Stop I2S. Blocks until TDM is stopped and reconfigured. */
void audio_i2s_deactivate(void);

uint32_t audio_i2s_tx_get_pending_bytes(void);
