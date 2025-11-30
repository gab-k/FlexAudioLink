/*
 * Copyright (c) 2015, Freescale Semiconductor, Inc.
 * Copyright 2016-2017 NXP
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* FreeRTOS kernel includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "timers.h"

/* Freescale includes. */
#include "fsl_device_registers.h"
#include "fsl_debug_console.h"
#include "board.h"
#include "app.h"

/* TinyUSB includes */
#include "tusb.h"


/*******************************************************************************
 * Definitions
 ******************************************************************************/
/* Task priorities. */
#define usb_device_task_PRIORITY (configMAX_PRIORITIES - 2)
#define cdc_app_task_PRIORITY (configMAX_PRIORITIES - 3)
#define hello_task_PRIORITY (configMAX_PRIORITIES - 4)

/*******************************************************************************
 * Prototypes
 ******************************************************************************/
static void hello_task(void *pvParameters);
static void usb_device_task(void *pvParameters);
static void cdc_app_task(void *pvParameters);

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
    
    // Create CDC application task.
    if (xTaskCreate(cdc_app_task, "cdc", 2048 / sizeof(StackType_t), NULL, cdc_app_task_PRIORITY, NULL) != pdPASS)
    {
        PRINTF("cdc task creation failed!.\r\n");
        while (1);
    }
    
    // Create hello task.
    // if (xTaskCreate(hello_task, "hello", configMINIMAL_STACK_SIZE + 100, NULL, hello_task_PRIORITY, NULL) != pdPASS)
    // {
    //     PRINTF("hello task creation failed!.\r\n");
    //     while (1);
    // }

    vTaskStartScheduler();
    while (1);
}

/*!
 * @brief Task responsible for printing of "Hello world." message.
 */
static void hello_task(void *pvParameters)
{
    for (;;)
    {
        PRINTF("Hello world.\r\n");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}


static void usb_device_task(void *pvParameters)
{
    // Init
    USB_DeviceClockInit();

    tusb_rhport_init_t dev_init = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_AUTO};
    tusb_init(BOARD_TUD_RHPORT, &dev_init);

    USB_DeviceIsrEnable();
    
    while (1)
    {
        tud_task(); // Blocks waiting for interrupt events
    }
}

static void cdc_app_task(void *pvParameters)
{
    while (1)
    {
        if (tud_cdc_connected())
        {
            if (tud_cdc_available())
            {
                uint8_t buf[64];
                uint32_t count = tud_cdc_read(buf, sizeof(buf));

                // Echo to Port 0
                tud_cdc_n_write(0, buf, count);
                tud_cdc_n_write_flush(0);

                // Echo to Port 1 (Dual CDC)
                tud_cdc_n_write(1, buf, count);
                tud_cdc_n_write_flush(1);
            }
        }
        
        // IMPORTANT: In FreeRTOS, you must yield or delay if nothing is happening
        // to allow lower priority tasks (if any) to run, though tud_cdc_read usually 
        // returns immediately if empty.
        // A small delay here saves power when idle.
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void board_get_unique_id(uint8_t id[], uint8_t max_len)
{
    // You can eventually read the real OCOTP unique ID here.
    // For now, we return a fixed dummy ID to make the build pass.
    for (uint8_t i = 0; i < max_len; i++)
    {
        id[i] = (uint8_t)(i + 1); 
    }
}