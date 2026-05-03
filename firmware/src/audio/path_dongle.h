#pragma once

#include <stdint.h>

#include "audio/path_common.h"

struct path_dongle_status {
	uint32_t overflow_bytes;
};

void path_dongle_init(void);
void path_dongle_get_status(struct path_dongle_status *out);