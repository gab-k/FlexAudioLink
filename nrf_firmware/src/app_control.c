#include "app_control.h"

#include "audio_io/audio_path_wired.h"
#include "audio_io/audio_path_wireless_dongle.h"
#include "audio_io/audio_path_wireless_headset.h"
#include "prop_gfsk/link.h"
#include "cli.h"
#include "prop_gfsk/test_mode.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/printk.h>

#define APP_CONTROL_THREAD_STACK_SIZE 2048
#define APP_CONTROL_THREAD_PRIORITY 9
#define APP_CONTROL_MSGQ_DEPTH 1

struct app_profile_set_request {
	enum app_profile profile;
	struct k_sem *done;
	bool *result;
};

static enum app_profile g_current_profile = APP_PROFILE_USB;

K_MSGQ_DEFINE(g_app_profile_queue, sizeof(struct app_profile_set_request), APP_CONTROL_MSGQ_DEPTH, 4);

static bool app_control_apply(enum app_profile profile)
{
	const bool target_is_pgfsk = (profile != APP_PROFILE_USB);
	const bool previous_is_pgfsk = (g_current_profile != APP_PROFILE_USB);
	const bool pgfsk_role_change = target_is_pgfsk && previous_is_pgfsk && profile != g_current_profile;

	if (pgfsk_role_change && pgfsk_test_mode_is_running()) {
		printk("app_control: stop linktest before changing PGFSK profile\n");
		return false;
	}

	if (target_is_pgfsk) {
		if (!pgfsk_test_mode_is_running()) {
			const bool started = (profile == APP_PROFILE_PGFSK_DONGLE)
						     ? pgfsk_link_start_dongle()
						     : pgfsk_link_start_headset();
			if (!started) {
				printk("app_control: failed to configure pgfsk link\n");
				return false;
			}
		}
	} else {
		if (pgfsk_test_mode_is_running() && !pgfsk_test_mode_stop()) {
			printk("app_control: failed to stop pgfsk test mode\n");
			return false;
		}
		pgfsk_link_stop();
	}

	g_current_profile = profile;

	audio_path_wired_deactivate();
	audio_path_wireless_dongle_deactivate();
	audio_path_wireless_headset_deactivate();

	if (profile == APP_PROFILE_USB) {
		audio_path_wired_activate();
	} else if (profile == APP_PROFILE_PGFSK_DONGLE) {
		audio_path_wireless_dongle_activate();
	} else {
		audio_path_wireless_headset_activate();
	}

	return true;
}

static void app_control_thread(void *arg1, void *arg2, void *arg3)
{
	struct app_profile_set_request request;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	if (settings_subsys_init()) {
		printk("app_control: settings_subsys_init failed\n");
	}

	uint8_t saved;
	ssize_t rc = settings_load_one("profile/active", &saved, sizeof(saved));
	if (rc == sizeof(saved) && saved < APP_PROFILE_COUNT) {
		g_current_profile = (enum app_profile)saved;
		printk("app_control: loaded saved profile %s\n", app_control_get_profile_name(g_current_profile));
	} else {
		g_current_profile = APP_PROFILE_USB;
	}

	if (!app_control_apply(g_current_profile)) {
		printk("app_control: failed to apply boot profile\n");
	}

	printk("Boot profile: %s\n", app_control_get_profile_name(g_current_profile));

	while (1) {
		(void)k_msgq_get(&g_app_profile_queue, &request, K_FOREVER);
		*request.result = app_control_apply(request.profile);
		if (*request.result) {
			uint8_t val = (uint8_t)g_current_profile;
			int ret = settings_save_one("profile/active", &val, sizeof(val));
			if (ret) {
				printk("app_control: failed to persist profile (err %d)\n", ret);
			}
		}
		k_sem_give(request.done);
	}
}

enum app_profile app_control_get_current_profile(void)
{
	return g_current_profile;
}

bool app_control_set_profile(enum app_profile profile)
{
	struct app_profile_set_request request;
	struct k_sem done;
	bool result;

	if (g_current_profile == profile) {
		return true;
	}

	k_sem_init(&done, 0, 1);
	request = (struct app_profile_set_request){
		.profile = profile,
		.done = &done,
		.result = &result,
	};
	(void)k_msgq_put(&g_app_profile_queue, &request, K_FOREVER);
	(void)k_sem_take(&done, K_FOREVER);
	return result;
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

K_THREAD_DEFINE(app_control_thread_id, APP_CONTROL_THREAD_STACK_SIZE,
		app_control_thread, NULL, NULL, NULL,
		APP_CONTROL_THREAD_PRIORITY, 0, 0);
