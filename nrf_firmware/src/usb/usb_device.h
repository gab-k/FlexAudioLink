#pragma once

#include <stdbool.h>

enum usb_device_profile {
	USB_DEVICE_PROFILE_CDC = 0,
	USB_DEVICE_PROFILE_UAC_CDC,
};

enum usb_device_profile usb_device_get_current_profile(void);
bool usb_device_request_profile(enum usb_device_profile profile);
