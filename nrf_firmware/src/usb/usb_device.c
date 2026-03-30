#include "usb/usb_device.h"

#include <zephyr/kernel.h>

#include "bsp/board_api.h"
#include "cli.h"
#include "tusb.h"
#include "usb/usb_cdc.h"

#define USB_THREAD_STACK_SIZE 2048
#define USB_THREAD_PRIORITY 5

#define USB_PROFILE_MSGQ_DEPTH 4

static enum usb_device_profile usb_device_current_profile = USB_DEVICE_PROFILE_CDC;
K_MSGQ_DEFINE(g_usb_profile_msgq, sizeof(enum usb_device_profile), USB_PROFILE_MSGQ_DEPTH, 4);

static void usb_device_thread(void *arg1, void *arg2, void *arg3);
static void usb_device_apply_profile(enum usb_device_profile profile);

enum usb_device_profile usb_device_get_current_profile(void)
{
	return usb_device_current_profile;
}

bool usb_device_request_profile(enum usb_device_profile profile)
{
	k_msgq_purge(&g_usb_profile_msgq);
	return k_msgq_put(&g_usb_profile_msgq, &profile, K_NO_WAIT) == 0;
}

static void usb_device_apply_profile(enum usb_device_profile profile)
{
	tusb_rhport_init_t dev_init = {
		.role = TUSB_ROLE_DEVICE,
		.speed = TUSB_SPEED_AUTO,
	};

	cli_set_connected(false);

	if (!tusb_inited()) {
		usb_device_current_profile = profile;
		tusb_init(BOARD_TUD_RHPORT, &dev_init);
		board_init_after_tusb();
		return;
	}

	tusb_deinit(BOARD_TUD_RHPORT);
	k_sleep(K_MSEC(20));
	usb_device_current_profile = profile;
	tusb_init(BOARD_TUD_RHPORT, &dev_init);
	board_init_after_tusb();
}

static void usb_device_thread(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	board_init();
	usb_cdc_init();

	while (1) {
		enum usb_device_profile requested_profile;

		if (!tusb_inited()) {
			usb_device_apply_profile(usb_device_current_profile);
		}

		if (k_msgq_get(&g_usb_profile_msgq, &requested_profile, K_NO_WAIT) == 0 &&
		    requested_profile != usb_device_current_profile) {
			usb_device_apply_profile(requested_profile);
		}

		tud_task();
		k_sleep(K_MSEC(1));
	}
}

void tud_mount_cb(void)
{
}

void tud_umount_cb(void)
{
	usb_cdc_on_unmount();
}

void tud_suspend_cb(bool remote_wakeup_en)
{
	(void)remote_wakeup_en;
}

void tud_resume_cb(void)
{
}

void board_init_after_tusb(void)
{
}

void board_reset_to_bootloader(void)
{
}

K_THREAD_DEFINE(usb_thread_id, USB_THREAD_STACK_SIZE, usb_device_thread,
		NULL, NULL, NULL, USB_THREAD_PRIORITY, 0, 0);
