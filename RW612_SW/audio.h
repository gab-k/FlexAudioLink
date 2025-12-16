#ifndef _AUDIO_H_
#define _AUDIO_H_

#include "tusb.h"
#include "usb_descriptors.h"
#include "peripherals.h"

void audio_task(void *pvParameters);
void audio_fb_task(void *pvParameters);
void I2S_TX_DMA_Callback(I2S_Type *base, i2s_dma_handle_t *handle, status_t completionStatus, void *userData);

extern dma_handle_t FLEXCOMM0_TX_Handle;
extern i2s_dma_handle_t FLEXCOMM0_Tx_DMA_Handle;

#endif /* _AUDIO_H_ */