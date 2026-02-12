/*
 * Copyright (c) 2015, Freescale Semiconductor, Inc.
 * Copyright 2016-2017 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

 /*******************************************************************************
 * Includes
 ******************************************************************************/
/* FreeRTOS kernel includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "timers.h"

/* Driver includes. */
#include "fsl_device_registers.h"
#include "fsl_debug_console.h"
#include "fsl_gpio.h"

/* Application includes. */
#include "pin_mux.h"
#include "board.h"
#include "app.h"
#include "audio.h"
#include "cli.h"
#include "wifi_app.h"
#include "udp.h"
#include "util.h"

/* TinyUSB includes */
#include "tusb.h"


/*******************************************************************************
 * Defines
 ******************************************************************************/
/* Task priorities. */
#define audio_task_PRIORITY (configMAX_PRIORITIES - 3)
#define audio_feedback_task_PRIORITY (configMAX_PRIORITIES - 4)
#define usb_device_task_PRIORITY (configMAX_PRIORITIES - 3)
#define led_blinking_task_PRIORITY (tskIDLE_PRIORITY)
#define wifi_task_PRIORITY (configMAX_PRIORITIES - 4)
#define udp_task_PRIORITY (configMAX_PRIORITIES - 3)
#define cli_task_PRIORITY (tskIDLE_PRIORITY)

/*******************************************************************************
 * Prototypes
 ******************************************************************************/
static void usb_device_task(void *pvParameters);

/*******************************************************************************
 * Variables
 ******************************************************************************/

/*******************************************************************************
 * Code
 ******************************************************************************/
/*!
 * @brief Application entry point.
 */
int main(void)
{
    /* Init board hardware. */
    BOARD_InitHardware();
    
    // Initialize UDP audio FIFOs in any case, even when UDP audio is not used.
    udp_audio_ff_init();

    // Create USB device task.
    if (xTaskCreate(usb_device_task, "usbd", 4096 / sizeof(StackType_t), NULL, usb_device_task_PRIORITY, NULL) != pdPASS)
    {
        PRINTF("usbd task creation failed!.\r\n");
        while (1);
    }

    // Create Wi-Fi task.
    if (xTaskCreate(wifi_task, "wifi", 4096 / sizeof(StackType_t), NULL, wifi_task_PRIORITY, &g_wifi_task_handle) != pdPASS)
    {
        PRINTF("wifi task creation failed!.\r\n");
        while (1);
    }
    vTaskSuspend(g_wifi_task_handle);
    g_wifi_events = xEventGroupCreate();

    // Create UDP task.
    if(xTaskCreate(udp_task, "udp", 32768 / sizeof(StackType_t), NULL, udp_task_PRIORITY, &g_udp_task_handle) != pdPASS)
    {
        PRINTF("udp task creation failed!.\r\n");
        while (1);
    }
    vTaskSuspend(g_udp_task_handle);

    // Create audio task.
    if (xTaskCreate(audio_task, "audio", 4096 / sizeof(StackType_t), NULL, audio_task_PRIORITY, &g_audio_task_handle) != pdPASS)
    {
        PRINTF("audio task creation failed!.\r\n");
        while (1);
    }
    vTaskSuspend(g_audio_task_handle);

    // Create audio feedback task.
    if (xTaskCreate(audio_fb_task, "audio_fb", 4096 / sizeof(StackType_t), NULL, audio_feedback_task_PRIORITY, &g_audio_fb_task_handle) != pdPASS)
    {
        PRINTF("audio feedback task creation failed!.\r\n");
        while (1);
    }
    vTaskSuspend(g_audio_fb_task_handle);

    // Create LED blinking task.
    if (xTaskCreate(blink_task, "blink", configMINIMAL_STACK_SIZE, NULL, led_blinking_task_PRIORITY, NULL) != pdPASS)
    {
        PRINTF("blink task creation failed!.\r\n");
        while (1);
    }

    // Create CLI task.
    if (xTaskCreate(cli_task, "cli", 2048 / sizeof(StackType_t), NULL, cli_task_PRIORITY, NULL) != pdPASS)
    {
        PRINTF("cli task creation failed!.\r\n");
        while (1);
    }


    vTaskStartScheduler();
    while (1);
}

static void usb_device_task(void *pvParameters)
{
    USB_DeviceClockInit();

    tusb_rhport_init_t dev_init = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_AUTO};
    tusb_init(BOARD_TUD_RHPORT, &dev_init);
    
    while (1)
    {
        tud_task();
    }
}