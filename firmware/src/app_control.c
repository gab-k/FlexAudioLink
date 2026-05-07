#include "app_control.h"

#include "audio/path_wired.h"
#include "audio/path_dongle.h"
#include "audio/path_headset.h"
#include "prop_fsk/session.h"

#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/printk.h>

#define APP_MODE_SETTINGS_KEY "mode/active"

static enum app_mode current_mode = APP_MODE_USB;
static K_SEM_DEFINE(boot_sem, 0, 1);

enum app_mode app_control_get_current_mode(void)
{
	return current_mode;
}

void app_control_await_boot(void)
{
	k_sem_take(&boot_sem, K_FOREVER);
}

const char *app_control_get_mode_name(enum app_mode mode)
{
	switch (mode) {
	case APP_MODE_USB:
		return "usb";
	case APP_MODE_PFSK_DONGLE:
		return "pfsk_dongle";
	case APP_MODE_PFSK_HEADSET:
		return "pfsk_headset";
	default:
		return "unknown";
	}
}

bool app_control_save_boot_mode(enum app_mode mode)
{
	if (mode >= APP_MODE_COUNT) {
		return false;
	}

	if (mode == current_mode) {
		return true;
	}

	uint8_t val = (uint8_t)mode;
	int rc = settings_save_one(APP_MODE_SETTINGS_KEY, &val, sizeof(val));
	if (rc) {
		printk("app_control: failed to persist mode (err %d)\n", rc);
		return false;
	}

	return true;
}

void app_control_boot(void)
{
	if (settings_subsys_init()) {
		printk("app_control: settings_subsys_init failed\n");
	}

	uint8_t saved;
	ssize_t rc = settings_load_one(APP_MODE_SETTINGS_KEY, &saved, sizeof(saved));
	if (rc == sizeof(saved) && saved < APP_MODE_COUNT) {
		current_mode = (enum app_mode)saved;
	} else {
		current_mode = APP_MODE_USB;
	}
	printk("Boot mode: %s\n", app_control_get_mode_name(current_mode));

	switch (current_mode) {
	case APP_MODE_USB:
		path_wired_init();
		break;
	case APP_MODE_PFSK_DONGLE:
		if (!pfsk_session_start_dongle()) {
			printk("app_control: failed to start PFSK dongle session\n");
		}
		path_dongle_init();
		break;
	case APP_MODE_PFSK_HEADSET:
		if (!pfsk_session_start_headset()) {
			printk("app_control: failed to start PFSK headset session\n");
		}
		path_headset_init();
		break;
	default:
		break;
	}

	k_sem_give(&boot_sem);
}