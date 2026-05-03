#pragma once

#include <zephyr/kernel.h>

enum app_profile {
	APP_PROFILE_USB,
	APP_PROFILE_PFSK_DONGLE,
	APP_PROFILE_PFSK_HEADSET,
	APP_PROFILE_COUNT,
};

void app_control_boot(void);
enum app_profile app_control_get_current_profile(void);
void app_control_await_boot(void);
bool app_control_save_boot_profile(enum app_profile profile);
const char *app_control_get_profile_name(enum app_profile profile);
