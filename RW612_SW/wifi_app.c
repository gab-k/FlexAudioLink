#include "wifi_app.h"

// Prototypes
static void link_status_change_cb(bool link_state);
static void wait_for_ip_address(int is_station_mode);

void wifi_task(void *pvParameters)
{
    wpl_ret_t err;
    // Initialize Wi-Fi driver and WPL layer
    PRINTF("\r\nInitializing Wi-Fi driver...\r\n");
    err = WPLRET_FAIL;
    err = WPL_Init();
    if (err != WPLRET_SUCCESS)
    {
        PRINTF("WPL_Init: Failed, error: %d\r\n", (uint32_t)err);
        while (1);
    }

    // Start Wi-Fi driver and register an application link state callback.
    PRINTF("\r\nStarting Wi-Fi driver...\r\n");
    err = WPLRET_FAIL;
    err = WPL_Start(link_status_change_cb);
    if (err != WPLRET_SUCCESS)
    {
        PRINTF("WPL_Start: Failed, error: %d\r\n", (uint32_t)err);
        while (1);
    }
    
    if (GPIO_PinRead(BOARD_INITPINS_SOFTAP_GPIO, BOARD_INITPINS_SOFTAP_PORT, BOARD_INITPINS_SOFTAP_PIN))
    {
        err = WPLRET_FAIL;
        err = WPL_Start_AP("WUMPA", "12345678", 36);
        if (err == WPLRET_SUCCESS) {
            PRINTF("SoftAP Started. Optimizing for Latency...\r\n");
            
            // Disable Aggregation on the AP interface (uAP)
            wlan_uap_ampdu_tx_disable();
            wlan_uap_ampdu_rx_disable();

            // Confirm our own IP (Should be 192.168.1.1)
            wait_for_ip_address(0); // 0 = AP Mode
            // Launch Server
            xTaskCreate(udp_server_task, "udp_rx", 2048 / sizeof(StackType_t), NULL, (configMAX_PRIORITIES - 1), NULL);
        } else {
            PRINTF("SoftAP Start Failed: %d\r\n", err);
        }
    }
    else
    {
        PRINTF("Adding default Wi-Fi Network...\r\n");
        err = WPLRET_FAIL;
        err = WPL_AddNetwork("WUMPA", "12345678", "def_network");
        if (err != WPLRET_SUCCESS)
        {
            PRINTF("WPL_AddNetwork: Failed, error: %d\r\n", (uint32_t)err);
        }
        
        PRINTF("Device joining the Wi-Fi Network using its STA interface...\r\n");
        err = WPLRET_FAIL;
        err = WPL_Join("def_network");
        if (err != WPLRET_SUCCESS)
        {
            PRINTF("WPL_Join: Failed, error: %d\r\n", (uint32_t)err);
        }
        else if (err == WPLRET_SUCCESS)
        {
            PRINTF("Connected. Optimizing for Latency...\r\n");
            // 1. Disable Power Save (Crucial: Stops AP from buffering data)
            wlan_ieeeps_off();

            // 2. Disable Aggregation (Crucial: Forces 1-packet-per-air-frame)
            //    This ensures your "Toggle High -> Toggle Low" matches exactly one frame.
            wlan_sta_ampdu_tx_disable(); // Don't merge outgoing packets
            wlan_sta_ampdu_rx_disable(); // Tell AP not to merge incoming packets

            wait_for_ip_address(1); // 1 = STA mode
            // Launch Client
            xTaskCreate(udp_client_task, "udp_tx", 2048 / sizeof(StackType_t), NULL, (configMAX_PRIORITIES - 1), NULL);
        }
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

static void wait_for_ip_address(int is_station_mode)
{
    char ip_str[32];
    wpl_ret_t ret;
    int role_param = is_station_mode ? 1 : 0; // 1=STA, 0=AP

    PRINTF("Waiting for IP address...\r\n");

    while (1)
    {
        // Try to get the IP
        ret = WPL_GetIP(ip_str, role_param);

        // Check if SUCCESS and if the string is not empty or "0.0.0.0"
        if (ret == WPLRET_SUCCESS && 
            strcmp(ip_str, "0.0.0.0") != 0 && 
            strlen(ip_str) > 0)
        {
            PRINTF("IP Address Assigned: %s\r\n", ip_str);
            //g_ip_acquired = true;
            break;
        }

        // Wait 500ms before retrying
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
