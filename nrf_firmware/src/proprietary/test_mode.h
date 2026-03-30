#pragma once

#include <stdbool.h>

#include "mode.h"

void proprietary_test_mode_init(void);
void proprietary_test_mode_set_enabled(bool enabled, enum device_role role);
bool proprietary_test_mode_is_enabled(void);
