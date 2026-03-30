#include "mode.h"

#include "cli.h"
#include "proprietary/link.h"
#include "proprietary/radio_hw.h"
#include "proprietary/test_mode.h"
#include "usb/usb_device.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define MODE_THREAD_STACK_SIZE 2048
#define MODE_THREAD_PRIORITY 9
#define MODE_MSGQ_DEPTH 4

#define MODE_DEFAULT_ROLE DEVICE_ROLE_HEADSET

static struct app_mode_state mode_current_state = {
	.role = DEVICE_ROLE_HEADSET,
	.mode = OPERATING_MODE_PROPRIETARY,
};
K_MSGQ_DEFINE(g_mode_msgq, sizeof(struct app_mode_state), MODE_MSGQ_DEPTH, 4);

static void mode_apply_state(struct app_mode_state state)
{
	enum usb_device_profile usb_profile =
		(state.mode == OPERATING_MODE_USB) ?
			USB_DEVICE_PROFILE_UAC_CDC :
			USB_DEVICE_PROFILE_CDC;
	bool proprietary_test_mode_enabled =
		(state.mode == OPERATING_MODE_PROPRIETARY);

	usb_device_request_profile(usb_profile);
	proprietary_test_mode_set_enabled(proprietary_test_mode_enabled,
					  state.role);
}

static void mode_announce_applied_state(struct app_mode_state state)
{
	char message[CLI_MAX_OUTPUT_LEN];
	int len;

	len = snprintf(message, sizeof(message),
		       "OK applied role=%s mode=%s\n",
		       mode_get_role_name(state.role),
		       mode_get_operating_mode_name(state.mode));
	if (len <= 0) {
		return;
	}

	cli_enqueue_print_msg(message);
}

static struct app_mode_state mode_get_latest_state(void)
{
	struct app_mode_state state = mode_current_state;

	(void)k_msgq_peek(&g_mode_msgq, &state);
	return state;
}

static bool mode_state_is_valid(struct app_mode_state state)
{
	if (state.role != DEVICE_ROLE_DONGLE &&
	    state.role != DEVICE_ROLE_HEADSET) {
		return false;
	}

	if (state.mode != OPERATING_MODE_PROPRIETARY &&
	    state.mode != OPERATING_MODE_BLE &&
	    state.mode != OPERATING_MODE_USB) {
		return false;
	}

	if (state.role == DEVICE_ROLE_DONGLE &&
	    state.mode == OPERATING_MODE_USB) {
		return false;
	}

	return true;
}

static void mode_enqueue_state(struct app_mode_state state)
{
	k_msgq_purge(&g_mode_msgq);
	(void)k_msgq_put(&g_mode_msgq, &state, K_NO_WAIT);
}

static bool mode_request_state(struct app_mode_state state)
{
	if (!mode_state_is_valid(state)) {
		return false;
	}

	mode_enqueue_state(state);
	return true;
}

static enum device_role mode_detect_default_role(void)
{
	uint8_t id[16];
	ssize_t len;
	uint64_t uid = 0;
	int copy_len;

	len = hwinfo_get_device_id(id, sizeof(id));
	if (len < 0) {
		printk("hwinfo_get_device_id failed: %zd\n", len);
		return MODE_DEFAULT_ROLE;
	}

	printk("Device UID (%zd bytes):", len);
	for (int i = 0; i < len; i++) {
		printk(" %02x", id[i]);
	}
	printk("\n");

	copy_len = (len < 8) ? len : 8;
	memcpy(&uid, id, copy_len);
	printk("Device UID (u64): 0x%016llx\n", uid);

	if (TX_DEVICE_ID != 0 && uid == TX_DEVICE_ID) {
		return DEVICE_ROLE_DONGLE;
	}

	return MODE_DEFAULT_ROLE;
}

static void mode_thread(void *arg1, void *arg2, void *arg3)
{
	struct app_mode_state requested_state;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	proprietary_link_init();
	proprietary_test_mode_init();

	mode_current_state = (struct app_mode_state){
		.role = mode_detect_default_role(),
		.mode = OPERATING_MODE_PROPRIETARY,
	};
	mode_apply_state(mode_current_state);

	printk("Default role: %s\n", mode_get_role_name(mode_current_state.role));
	printk("Default mode: %s\n",
	       mode_get_operating_mode_name(mode_current_state.mode));

	while (1) {
		if (k_msgq_get(&g_mode_msgq, &requested_state, K_FOREVER) != 0) {
			continue;
		}

		if (requested_state.role == mode_current_state.role &&
		    requested_state.mode == mode_current_state.mode) {
			continue;
		}

		mode_apply_state(requested_state);
		mode_current_state = requested_state;
		mode_announce_applied_state(mode_current_state);
	}
}

struct app_mode_state mode_get_current_state(void)
{
	return mode_current_state;
}

enum device_role mode_get_current_role(void)
{
	return mode_get_current_state().role;
}

enum operating_mode mode_get_current_operating_mode(void)
{
	return mode_get_current_state().mode;
}

bool mode_request_role(enum device_role role)
{
	struct app_mode_state state;

	state = mode_get_latest_state();

	if (state.role == role) {
		return true;
	}

	state.role = role;

	if (state.role == DEVICE_ROLE_DONGLE &&
	    state.mode == OPERATING_MODE_USB) {
		state.mode = OPERATING_MODE_PROPRIETARY;
	}

	return mode_request_state(state);
}

bool mode_request_operating_mode(enum operating_mode mode)
{
	struct app_mode_state state;

	state = mode_get_latest_state();

	if (state.mode == mode) {
		return true;
	}

	state.mode = mode;
	return mode_request_state(state);
}

const char *mode_get_role_name(enum device_role role)
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

const char *mode_get_operating_mode_name(enum operating_mode mode)
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

K_THREAD_DEFINE(mode_thread_id, MODE_THREAD_STACK_SIZE, mode_thread,
		NULL, NULL, NULL, MODE_THREAD_PRIORITY, 0, 0);
