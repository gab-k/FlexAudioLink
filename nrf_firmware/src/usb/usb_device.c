#include "usb/usb_device.h"

#include <zephyr/devicetree.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>

#include <nrfx.h>
#include <nrfx_glue.h>

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
static void usb_device_low_level_init(void);
static void usbhs_isr(const void *arg);

enum usb_device_profile usb_device_get_current_profile(void)
{
	return usb_device_current_profile;
}

bool usb_device_request_profile(enum usb_device_profile profile)
{
	k_msgq_purge(&g_usb_profile_msgq);
	return k_msgq_put(&g_usb_profile_msgq, &profile, K_NO_WAIT) == 0;
}

static void usbhs_isr(const void *arg)
{
	ARG_UNUSED(arg);

	tusb_int_handler(BOARD_TUD_RHPORT, true);
}

static void usb_device_low_level_init(void)
{
	static bool initialized;

	if (initialized) {
		return;
	}

	IRQ_CONNECT(DT_IRQN(DT_NODELABEL(usbhs)),
		    DT_IRQ(DT_NODELABEL(usbhs), priority),
		    usbhs_isr, NULL, 0);
	irq_enable(DT_IRQN(DT_NODELABEL(usbhs)));

	NRF_VREGUSB->TASKS_START = VREGUSB_TASKS_START_TASKS_START_Trigger;

	NRF_CLOCK->TASKS_XO24MSTART = CLOCK_TASKS_XO24MSTART_TASKS_XO24MSTART_Trigger;
	while (!NRF_CLOCK->EVENTS_XO24MSTARTED) {
	}
	NRF_CLOCK->EVENTS_XO24MSTARTED = 0;

	/* Based on Zephyr usbhs_enable_core() in drivers/usb/udc/udc_dwc2_vendor_quirks.h. */
	NRF_USBHS->ENABLE = USBHS_ENABLE_CORE_Msk;
	NRF_USBHS->PHY.OVERRIDEVALUES =
		(USBHS_PHY_OVERRIDEVALUES_ID_Device << USBHS_PHY_OVERRIDEVALUES_ID_Pos);
	NRF_USBHS->PHY.INPUTOVERRIDE =
		USBHS_PHY_INPUTOVERRIDE_ID_Msk | USBHS_PHY_INPUTOVERRIDE_VBUSVALID_Msk;
	NRF_USBHS->ENABLE = USBHS_ENABLE_PHY_Msk | USBHS_ENABLE_CORE_Msk;
	NRFX_DELAY_US(45);
	NRF_USBHS->TASKS_START = USBHS_TASKS_START_TASKS_START_Trigger;
	NRFX_DELAY_US(2);
	NRF_USBHS->PHY.INPUTOVERRIDE = USBHS_PHY_INPUTOVERRIDE_ID_Msk;
	__DSB();

	NVIC_SetPriority(USBHS_IRQn, 2);

	initialized = true;
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
		return;
	}

	tusb_deinit(BOARD_TUD_RHPORT);
	k_sleep(K_MSEC(20));
	usb_device_current_profile = profile;
	tusb_init(BOARD_TUD_RHPORT, &dev_init);
}

static void usb_device_thread(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	usb_device_low_level_init();
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

void tud_umount_cb(void)
{
	usb_cdc_on_unmount();
}

K_THREAD_DEFINE(usb_thread_id, USB_THREAD_STACK_SIZE, usb_device_thread,
		NULL, NULL, NULL, USB_THREAD_PRIORITY, 0, 0);
