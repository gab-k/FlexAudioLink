/* usb_audio_user.h */
#ifndef __AUDIO_H__
#define __AUDIO_H__

#include <stdint.h>
#include "main.h"
//#include <stdbool.h>

// Expose the blink interval so main.c can toggle LEDs based on USB state
extern uint32_t blink_interval_ms;

// Task prototypes
void audio_init(void);
void init_test_sawtooth(void);
void init_test_sine_array(void);
void audio_task(void);
void audio_control_task(void);

void audio_init_test();
void audio_start_test();

void I2S_DMA_TX_HalfCpltCallback(DMA_HandleTypeDef *hi2s);
void I2S_DMA_TX_CpltCallback(DMA_HandleTypeDef *hi2s);
void I2S_DMA_TX_ErrorCallback(DMA_HandleTypeDef *hi2s);

void I2S_DMA_RX_HalfCpltCallback(DMA_HandleTypeDef *hi2s);
void I2S_DMA_RX_CpltCallback(DMA_HandleTypeDef *hi2s);
void I2S_DMA_RX_ErrorCallback(DMA_HandleTypeDef *hi2s);


#endif /* __AUDIO_H__ */

  