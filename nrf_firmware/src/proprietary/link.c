#include "proprietary/link.h"

static enum proprietary_link_mode g_link_mode = PROPRIETARY_LINK_MODE_OFF;

void proprietary_link_init(void)
{
	radio_hw_init();
	g_link_mode = PROPRIETARY_LINK_MODE_OFF;
}

bool proprietary_link_open(const struct proprietary_link_config *config)
{
	if (config == NULL) {
		return false;
	}

	g_link_mode = config->mode;

	switch (config->mode) {
	case PROPRIETARY_LINK_MODE_TX:
		radio_hw_set_runtime_mode(RADIO_RUNTIME_TX);
		return true;
	case PROPRIETARY_LINK_MODE_RX:
		radio_hw_set_runtime_mode(RADIO_RUNTIME_RX);
		return true;
	case PROPRIETARY_LINK_MODE_OFF:
	default:
		radio_hw_set_runtime_mode(RADIO_RUNTIME_OFF);
		return true;
	}
}

void proprietary_link_close(void)
{
	g_link_mode = PROPRIETARY_LINK_MODE_OFF;
	radio_hw_set_runtime_mode(RADIO_RUNTIME_OFF);
}

enum proprietary_link_mode proprietary_link_get_mode(void)
{
	return g_link_mode;
}

void proprietary_link_get_stats(struct radio_stats *stats)
{
	radio_hw_get_stats(stats);
}
