#ifndef _MODE_H_
#define _MODE_H_

#include "FreeRTOS.h"
#include "task.h"
#include <stdbool.h>
#include "tusb.h"
#include "usb_descriptors.h"
#include "udp.h"
#include "raw_audio.h"
#include "wifi_app.h"
#include "audio.h"

typedef enum app_mode_t {
    MODE_IDLE,
    MODE_USB_AUDIO,
    MODE_UDP_DONGLE_AUDIO,
    MODE_UDP_DONGLE_TONE,
    MODE_UDP_HEADSET_AUDIO,
    MODE_RAW_DONGLE_AUDIO,
    MODE_RAW_DONGLE_TONE,
    MODE_RAW_HEADSET_AUDIO,
    MODE_BLE_AUDIO
} app_mode_t;

const char* get_app_mode_name(app_mode_t mode);
bool set_current_app_mode(app_mode_t new_mode);
app_mode_t get_app_mode(void);
usb_desc_profile_t get_usb_profile_for_mode(app_mode_t mode);
void get_active_fifos(tu_fifo_t **spk_ff_ptr, tu_fifo_t **mic_ff_ptr);



#endif // _MODE_H_