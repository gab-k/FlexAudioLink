#include "app_control.h"

#include "audio_io/audio_path_wired.h"
#include "audio_io/audio_path_wireless_dongle.h"
#include "audio_io/audio_path_wireless_headset.h"
#include "prop_gfsk/link.h"

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>

#define APP_PROFILE_SETTINGS_KEY "profile/active"

static enum app_profile g_current_profile = APP_PROFILE_USB;

enum app_profile app_control_get_current_profile(void)
{
	return g_current_profile;
}

const char *app_control_get_profile_name(enum app_profile profile)
{
	switch (profile) {
	case APP_PROFILE_USB:
		return "usb";
	case APP_PROFILE_PGFSK_DONGLE:
		return "pgfsk_dongle";
	case APP_PROFILE_PGFSK_HEADSET:
		return "pgfsk_headset";
	default:
		return "unknown";
	}
}

void app_control_set_profile(enum app_profile profile)
{
	if (profile >= APP_PROFILE_COUNT) {
		return;
	}

	if (profile == g_current_profile) {
		return;
	}

	uint8_t val = (uint8_t)profile;
	int rc = settings_save_one(APP_PROFILE_SETTINGS_KEY, &val, sizeof(val));
	if (rc) {
		printk("app_control: failed to persist profile (err %d)\n", rc);
		return;
	}

	sys_reboot(SYS_REBOOT_COLD);
}

void app_control_boot(void)
{
	if (settings_subsys_init()) {
		printk("app_control: settings_subsys_init failed\n");
	}

	uint8_t saved;
	ssize_t rc = settings_load_one(APP_PROFILE_SETTINGS_KEY, &saved, sizeof(saved));
	if (rc == sizeof(saved) && saved < APP_PROFILE_COUNT) {
		g_current_profile = (enum app_profile)saved;
	} else {
		g_current_profile = APP_PROFILE_USB;
	}

	printk("Boot profile: %s\n", app_control_get_profile_name(g_current_profile));

	switch (g_current_profile) {
	case APP_PROFILE_USB:
		audio_path_wired_init();
		break;
	case APP_PROFILE_PGFSK_DONGLE:
		if (!pgfsk_link_start_dongle()) {
			printk("app_control: failed to start pgfsk dongle link\n");
		}
		audio_path_wireless_dongle_init();
		break;
	case APP_PROFILE_PGFSK_HEADSET:
		if (!pgfsk_link_start_headset()) {
			printk("app_control: failed to start pgfsk headset link\n");
		}
		audio_path_wireless_headset_init();
		break;
	default:
		break;
	}
}
