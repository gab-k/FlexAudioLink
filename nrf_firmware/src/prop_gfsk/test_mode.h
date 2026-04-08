#pragma once

#include <stdbool.h>

#include "app_control.h"

bool prop_gfsk_test_mode_start(enum device_role local_device_role);
bool prop_gfsk_test_mode_stop(void);
bool prop_gfsk_test_mode_is_running(void);
