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

struct app_mode_state {
	enum device_role role;
	enum operating_mode mode;
};

struct app_mode_state mode_get_current_state(void);
enum device_role mode_get_current_role(void);
enum operating_mode mode_get_current_operating_mode(void);
bool mode_request_role(enum device_role role);
bool mode_request_operating_mode(enum operating_mode mode);
const char *mode_get_role_name(enum device_role role);
const char *mode_get_operating_mode_name(enum operating_mode mode);
