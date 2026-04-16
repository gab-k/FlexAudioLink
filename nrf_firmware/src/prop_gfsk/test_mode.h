#pragma once

#include <stdbool.h>

#include "app_control.h"

bool pgfsk_test_mode_start(enum device_role local_device_role);
bool pgfsk_test_mode_stop(void);
bool pgfsk_test_mode_is_running(void);
