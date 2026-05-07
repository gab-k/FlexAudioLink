#pragma once

#include <zephyr/kernel.h>

enum app_mode {
	APP_MODE_USB,
	APP_MODE_PROP_DONGLE,
	APP_MODE_PROP_HEADSET,
	APP_MODE_COUNT,
};

void app_control_boot(void);
enum app_mode app_control_get_current_mode(void);
void app_control_await_boot(void);
bool app_control_save_boot_mode(enum app_mode mode);
const char *app_control_get_mode_name(enum app_mode mode);
