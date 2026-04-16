#include "app_control.h"

#include "cli.h"
#include "prop_gfsk/test_mode.h"
#include "usb/usb_device.h"

#include <string.h>

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define APP_CONTROL_THREAD_STACK_SIZE 2048
#define APP_CONTROL_THREAD_PRIORITY 9
#define APP_CONTROL_MSGQ_DEPTH 1
#define APP_CONTROL_SET_TIMEOUT K_MSEC(1000)

#define APP_CONTROL_DEFAULT_ROLE DEVICE_ROLE_HEADSET
#define APP_CONTROL_DONGLE_DEVICE_UID 0xb007ec2fe3f4e0c0ULL

struct app_state {
	enum device_role role;
	enum operating_mode mode;
};

struct app_state_set_request {
	struct app_state state;
	struct k_sem *done;
	bool *result;
};

static struct app_state g_current_app_state = {
	.role = DEVICE_ROLE_HEADSET,
	.mode = OPERATING_MODE_PROPRIETARY,
};

K_MSGQ_DEFINE(g_app_state_queue, sizeof(struct app_state_set_request), APP_CONTROL_MSGQ_DEPTH, 4);
K_MUTEX_DEFINE(g_app_state_set_lock);

static bool app_control_apply(struct app_state state)
{
	enum usb_device_profile usb_profile =
		(state.mode == OPERATING_MODE_USB) ?
			USB_DEVICE_PROFILE_UAC_CDC :
			USB_DEVICE_PROFILE_CDC;
	bool role_changed = state.role != g_current_app_state.role;

	if (state.mode == OPERATING_MODE_PROPRIETARY &&
	    g_current_app_state.mode == OPERATING_MODE_PROPRIETARY &&
	    role_changed &&
	    pgfsk_test_mode_is_running()) {
		printk("app_control: stop linktest before changing role in proprietary mode\n");
		return false;
	}

	if (state.mode != OPERATING_MODE_PROPRIETARY) {
		if (!pgfsk_test_mode_stop()) {
			printk("app_control: failed to disable pgfsk test mode\n");
			return false;
		}
	}

	usb_device_request_profile(usb_profile);
	return true;
}

static bool app_control_is_valid(struct app_state state)
{
	return !(state.role == DEVICE_ROLE_DONGLE && state.mode == OPERATING_MODE_USB);
}

static void app_control_enqueue(const struct app_state_set_request *request)
{
	k_msgq_purge(&g_app_state_queue);
	(void)k_msgq_put(&g_app_state_queue, request, K_FOREVER);
}

static enum device_role app_control_detect_default_role(void)
{
	uint8_t id[16];
	ssize_t len;
	uint64_t uid = 0;
	int copy_len;

	len = hwinfo_get_device_id(id, sizeof(id));
	if (len < 0) {
		printk("hwinfo_get_device_id failed: %zd\n", len);
		return APP_CONTROL_DEFAULT_ROLE;
	}

	printk("Device UID (%zd bytes):", len);
	for (int i = 0; i < len; i++) {
		printk(" %02x", id[i]);
	}
	printk("\n");

	copy_len = (len < 8) ? len : 8;
	memcpy(&uid, id, copy_len);
	printk("Device UID (u64): 0x%016llx\n", uid);

	if (APP_CONTROL_DONGLE_DEVICE_UID != 0 &&
	    uid == APP_CONTROL_DONGLE_DEVICE_UID) {
		return DEVICE_ROLE_DONGLE;
	}

	return APP_CONTROL_DEFAULT_ROLE;
}

static void app_control_thread(void *arg1, void *arg2, void *arg3)
{
	struct app_state_set_request request;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	g_current_app_state = (struct app_state){
		.role = app_control_detect_default_role(),
		.mode = OPERATING_MODE_PROPRIETARY,
	};
	if (!app_control_apply(g_current_app_state)) {
		printk("app_control: failed to apply default state\n");
	}

	printk("Default role: %s\n", app_control_get_role_name(g_current_app_state.role));
	printk("Default mode: %s\n", app_control_get_operating_mode_name(g_current_app_state.mode));

	while (1) {
		if (k_msgq_get(&g_app_state_queue, &request, K_FOREVER) != 0) {
			continue;
		}

		if (!app_control_apply(request.state)) {
			*request.result = false;
			k_sem_give(request.done);
			continue;
		}

		g_current_app_state = request.state;
		*request.result = true;
		k_sem_give(request.done);
	}
}

enum device_role app_control_get_current_role(void)
{
	return g_current_app_state.role;
}

enum operating_mode app_control_get_current_operating_mode(void)
{
	return g_current_app_state.mode;
}

bool app_control_set(enum device_role role, enum operating_mode mode)
{
	struct app_state state = {
		.role = role,
		.mode = mode,
	};
	struct app_state_set_request request;
	struct k_sem done;
	bool result;

	if (!app_control_is_valid(state)) {
		return false;
	}

	k_mutex_lock(&g_app_state_set_lock, K_FOREVER);
	if (g_current_app_state.role == role && g_current_app_state.mode == mode) {
		k_mutex_unlock(&g_app_state_set_lock);
		return true;
	}

	k_sem_init(&done, 0, 1);
	request = (struct app_state_set_request){
		.state = state,
		.done = &done,
		.result = &result,
	};
	app_control_enqueue(&request);
	if (k_sem_take(&done, APP_CONTROL_SET_TIMEOUT) != 0) {
		printk("app_control: timed out waiting for state apply\n");
		k_mutex_unlock(&g_app_state_set_lock);
		return false;
	}
	k_mutex_unlock(&g_app_state_set_lock);
	return result;
}

const char *app_control_get_role_name(enum device_role role)
{
	switch (role) {
	case DEVICE_ROLE_DONGLE:
		return "dongle";
	case DEVICE_ROLE_HEADSET:
		return "headset";
	default:
		return "unknown";
	}
}

const char *app_control_get_operating_mode_name(enum operating_mode mode)
{
	switch (mode) {
	case OPERATING_MODE_PROPRIETARY:
		return "proprietary";
	case OPERATING_MODE_BLE:
		return "ble";
	case OPERATING_MODE_USB:
		return "usb";
	default:
		return "unknown";
	}
}

K_THREAD_DEFINE(app_control_thread_id, APP_CONTROL_THREAD_STACK_SIZE,
		app_control_thread, NULL, NULL, NULL,
		APP_CONTROL_THREAD_PRIORITY, 0, 0);
