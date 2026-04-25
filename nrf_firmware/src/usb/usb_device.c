#include <zephyr/devicetree.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>

#include <nrfx.h>
#include <nrfx_glue.h>
#include <zephyr/sys/sys_io.h>

#include "cli.h"
#include "tusb.h"
#include "usb/usb_cdc.h"
#include "usb/usb_audio.h"

#define USB_THREAD_STACK_SIZE 2048
#define USB_THREAD_PRIORITY 5

static void usb_device_thread(void *arg1, void *arg2, void *arg3);
static void usb_device_start(void);
static bool usb_device_low_level_init(void);
static void usbhs_isr(const void *arg);

static void usbhs_isr(const void *arg)
{
	ARG_UNUSED(arg);
	tusb_int_handler(BOARD_TUD_RHPORT, true);
}

/* VBUS present status: undocumented VREGUSB status register bit, sourced from
 * Zephyr's udc_dwc2_vendor_quirks.h. */
#define VREGUSB_STATUS_VBUS_PRESENT_BIT BIT(2)
#define VREGUSB_STATUS_REG_OFFSET       0x400

static bool usb_device_vbus_present(void)
{
	return sys_read32((mem_addr_t)NRF_VREGUSB + VREGUSB_STATUS_REG_OFFSET) & VREGUSB_STATUS_VBUS_PRESENT_BIT;
}

static bool usb_device_low_level_init(void)
{
	static bool irq_registered;
	static bool initialized;

	if (initialized) {
		return true;
	}

	if (!irq_registered) {
		IRQ_CONNECT(DT_IRQN(DT_NODELABEL(usbhs)),
			    DT_IRQ(DT_NODELABEL(usbhs), priority),
			    usbhs_isr, NULL, 0);
		NRF_VREGUSB->TASKS_START = VREGUSB_TASKS_START_TASKS_START_Trigger;
		irq_registered = true;
	}

	if (!usb_device_vbus_present()) {
		return false;
	}

	irq_enable(DT_IRQN(DT_NODELABEL(usbhs)));

	NRF_CLOCK->TASKS_XO24MSTART = CLOCK_TASKS_XO24MSTART_TASKS_XO24MSTART_Trigger;
	while (!NRF_CLOCK->EVENTS_XO24MSTARTED) {
	}
	NRF_CLOCK->EVENTS_XO24MSTARTED = 0;

	/* Based on Zephyr usbhs_enable_core() in drivers/usb/udc/udc_dwc2_vendor_quirks.h. */
	NRF_USBHS->ENABLE = USBHS_ENABLE_CORE_Msk;
	NRF_USBHS->PHY.OVERRIDEVALUES = (USBHS_PHY_OVERRIDEVALUES_ID_Device << USBHS_PHY_OVERRIDEVALUES_ID_Pos);
	NRF_USBHS->PHY.INPUTOVERRIDE = USBHS_PHY_INPUTOVERRIDE_ID_Msk | USBHS_PHY_INPUTOVERRIDE_VBUSVALID_Msk;
	NRF_USBHS->ENABLE = USBHS_ENABLE_PHY_Msk | USBHS_ENABLE_CORE_Msk;
	NRFX_DELAY_US(45);
	NRF_USBHS->TASKS_START = USBHS_TASKS_START_TASKS_START_Trigger;
	NRFX_DELAY_US(2);
	NRF_USBHS->PHY.INPUTOVERRIDE = USBHS_PHY_INPUTOVERRIDE_ID_Msk;
	__DSB();

	NVIC_SetPriority(USBHS_IRQn, 2);

	initialized = true;
	return true;
}

static void usb_device_start(void)
{
	tusb_rhport_init_t dev_init = {
		.role = TUSB_ROLE_DEVICE,
		.speed = TUSB_SPEED_AUTO,
	};

	cli_set_connected(false);
	usb_audio_reset();
	tusb_init(BOARD_TUD_RHPORT, &dev_init);
	usb_cdc_init();
}

static void usb_device_thread(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (1) {
		if (!usb_device_low_level_init()) {
			k_sleep(K_MSEC(100));
			continue;
		}

		if (!tusb_inited()) {
			usb_device_start();
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
