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

/* Freescale includes. */
#include "fsl_device_registers.h"
#include "fsl_debug_console.h"
#include "fsl_gpio.h"

#include "pin_mux.h"
#include "board.h"
#include "app.h"
#include "audio.h"
#include "cli.h"

#include "wpl.h"

/* TinyUSB includes */
#include "tusb.h"


/*******************************************************************************
 * Defines
 ******************************************************************************/
/* Task priorities. */
#define audio_task_PRIORITY (configMAX_PRIORITIES - 1)
#define audio_feedback_task_PRIORITY (configMAX_PRIORITIES - 4)
#define usb_device_task_PRIORITY (configMAX_PRIORITIES - 1)
#define led_blinking_task_PRIORITY (tskIDLE_PRIORITY)
#define wifi_task_PRIORITY (configMAX_PRIORITIES - 4)
#define cli_task_PRIORITY (tskIDLE_PRIORITY)

/*******************************************************************************
 * Prototypes
 ******************************************************************************/
static void usb_device_task(void *pvParameters);
static void wifi_task(void *pvParameters);
static void link_status_change_cb(bool link_state);
static void blink_task(void *pvParameters);

/*******************************************************************************
 * Variables
 ******************************************************************************/
extern uint32_t blink_interval_ms;

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
    
    // Create USB device task.
    if (xTaskCreate(usb_device_task, "usbd", 4096 / sizeof(StackType_t), NULL, usb_device_task_PRIORITY, NULL) != pdPASS)
    {
        PRINTF("usbd task creation failed!.\r\n");
        while (1);
    }

    // Create Wi-Fi task.
    if (xTaskCreate(wifi_task, "wifi", 4096 / sizeof(StackType_t), NULL, wifi_task_PRIORITY, NULL) != pdPASS)
    {
        PRINTF("wifi task creation failed!.\r\n");
        while (1);
    }

    // Create audio task.
    if (xTaskCreate(audio_task, "audio", 4096 / sizeof(StackType_t), NULL, audio_task_PRIORITY, NULL) != pdPASS)
    {
        PRINTF("audio task creation failed!.\r\n");
        while (1);
    }

    // Create audio feedback task.
    if (xTaskCreate(audio_fb_task, "audio_fb", 4096 / sizeof(StackType_t), NULL, audio_feedback_task_PRIORITY, NULL) != pdPASS)
    {
        PRINTF("audio feedback task creation failed!.\r\n");
        while (1);
    }

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

static void wifi_task(void *pvParameters)
{
    // Initialize Wi-Fi driver and WPL layer
    PRINTF("\r\nInitializing Wi-Fi driver...\r\n");
    wpl_ret_t err = WPLRET_FAIL;
    err = WPL_Init();
    if (err != WPLRET_SUCCESS)
    {
        PRINTF("WPL_Init: Failed, error: %d\r\n", (uint32_t)err);
        while (1);
    }

    // Start Wi-Fi driver and register an application link state callback.
    PRINTF("\r\nStarting Wi-Fi driver...\r\n");
    err = WPL_Start(link_status_change_cb);
    if (err != WPLRET_SUCCESS)
    {
        PRINTF("WPL_Start: Failed, error: %d\r\n", (uint32_t)err);
        while (1);
    }

    vTaskSuspend(NULL);
}

static void link_status_change_cb(bool link_state)
{
    if (link_state == false)
    {
        PRINTF("-------- LINK LOST --------\r\n");
    }
    else
    {
        PRINTF("-------- LINK REESTABLISHED --------\r\n");
    }
}

static void blink_task(void *pvParameters) 
{
    TickType_t delay_ticks;
    while(1)
    {
        // Toggle First
        GPIO_PortToggle(GPIO, BOARD_INITLEDPINS_LED_GREEN_PORT, BOARD_INITLEDPINS_LED_GREEN_PIN_MASK);

        // Delay for blink_interval_ms milliseconds
        delay_ticks = pdMS_TO_TICKS(blink_interval_ms);
        configASSERT(delay_ticks > 0);
        vTaskDelay(delay_ticks);
    }
}
  

void board_get_unique_id(uint8_t id[], uint8_t max_len)
{
    // For now, return a fixed dummy ID to make the build pass.
    for (uint8_t i = 0; i < max_len; i++)
    {
        id[i] = (uint8_t)(i + 1); 
    }
}