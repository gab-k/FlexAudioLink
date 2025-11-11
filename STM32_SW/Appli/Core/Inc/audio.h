/* usb_audio_user.h */
#ifndef __AUDIO_H__
#define __AUDIO_H__

#include <stdint.h>
//#include <stdbool.h>

// Expose the blink interval so main.c can toggle LEDs based on USB state
extern uint32_t blink_interval_ms;

// Task prototypes
void audio_init_sine(void);
void audio_task(void);
void audio_control_task(void);

#endif /* __AUDIO_H__ */

  