/*
 * Copyright 2021 NXP.
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*${header:start}*/
// #include "usb_device_config.h"
// #include "usb.h"
// #include "usb_device.h"
// #include "usb_device_class.h"
// #include "usb_device_ch9.h"
// #include "usb_device_descriptor.h"
// #include "virtual_com.h"

#include "pin_mux.h"
#include "clock_config.h"
#include "board.h"
#include "tusb.h"

#define USB_DEVICE_INTERRUPT_PRIORITY (6U)

/*${header:end}*/

//extern usb_cdc_vcom_struct_t s_cdcVcom;

/*${function:start}*/
void BOARD_InitHardware(void)
{
    BOARD_InitPins();
    BOARD_BootClockRUN();
    BOARD_InitDebugConsole();
}

void USB_DeviceClockInit(void)
{
    /* reset USB */
    RESET_PeripheralReset(kUSB_RST_SHIFT_RSTn);
    /* enable usb clock */
    CLOCK_EnableClock(kCLOCK_Usb);
    /* enable usb phy clock */
    CLOCK_EnableUsbhsPhyClock();
}

void USB_DeviceIsrEnable(void)
{
    uint8_t irqNumber = USB_IRQn;
    /* Install isr, set priority, and enable IRQ. */
    NVIC_SetPriority((IRQn_Type)irqNumber, USB_DEVICE_INTERRUPT_PRIORITY);
    EnableIRQ((IRQn_Type)irqNumber);
}

void USBHS_IRQHandler(void)
{
    tusb_int_handler(BOARD_TUD_RHPORT, 1);
}
/*${function:end}*/
