#ifndef _AUDIO_H_
#define _AUDIO_H_

#include "tusb.h"
#include "usb_descriptors.h"
#include "peripherals.h"
#include "mode.h"

void audio_task(void *pvParameters);
void audio_fb_task(void *pvParameters);
void I2S_TX_DMA_Callback(I2S_Type *base, i2s_dma_handle_t *handle, status_t completionStatus, void *userData);
void audio_reset_state(void);

extern TaskHandle_t g_audio_task_handle;
extern TaskHandle_t g_audio_fb_task_handle;
extern dma_handle_t FLEXCOMM0_TX_Handle;
extern i2s_dma_handle_t FLEXCOMM0_Tx_DMA_Handle;
extern uint32_t g_blink_interval_ms;

#endif /* _AUDIO_H_ */