#pragma once

enum app_profile {
	APP_PROFILE_USB,
	APP_PROFILE_PGFSK_DONGLE,
	APP_PROFILE_PGFSK_HEADSET,
	APP_PROFILE_COUNT,
};

void app_control_boot(void);
enum app_profile app_control_get_current_profile(void);
void app_control_set_profile(enum app_profile profile);
const char *app_control_get_profile_name(enum app_profile profile);
