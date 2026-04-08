#pragma once

#include <stdbool.h>
#include <stdint.h>

enum device_role {
	DEVICE_ROLE_DONGLE,
	DEVICE_ROLE_HEADSET,
};

enum operating_mode {
	OPERATING_MODE_PROPRIETARY,
	OPERATING_MODE_BLE,
	OPERATING_MODE_USB,
};

enum device_role app_control_get_current_role(void);
enum operating_mode app_control_get_current_operating_mode(void);
bool app_control_set(enum device_role role, enum operating_mode mode);
const char *app_control_get_role_name(enum device_role role);
const char *app_control_get_operating_mode_name(enum operating_mode mode);
