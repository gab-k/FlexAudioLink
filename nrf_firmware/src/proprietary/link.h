#pragma once

#include <stdbool.h>

#include "proprietary/radio_hw.h"

enum proprietary_link_mode {
	PROPRIETARY_LINK_MODE_OFF = 0,
	PROPRIETARY_LINK_MODE_TX,
	PROPRIETARY_LINK_MODE_RX,
};

struct proprietary_link_config {
	enum proprietary_link_mode mode;
};

void proprietary_link_init(void);
bool proprietary_link_open(const struct proprietary_link_config *config);
void proprietary_link_close(void);
enum proprietary_link_mode proprietary_link_get_mode(void);
void proprietary_link_get_stats(struct radio_stats *stats);
