#ifndef _MODE_H_
#define _MODE_H_

#include "FreeRTOS.h"
#include "task.h"
#include <stdbool.h>
#include "tusb.h"
#include "usb_descriptors.h"

typedef enum app_mode_t {
    MODE_IDLE,
    MODE_USB_AUDIO,
    MODE_UDP_DONGLE_AUDIO,
    MODE_UDP_HEADSET_AUDIO,
    MODE_BLE_AUDIO
} app_mode_t;

const char* get_app_mode_name(app_mode_t mode);
bool set_current_app_mode(app_mode_t new_mode);
app_mode_t get_app_mode(void);
usb_desc_profile_t get_usb_profile_for_mode(app_mode_t mode);



#endif // _MODE_H_