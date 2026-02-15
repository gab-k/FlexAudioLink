#include "wifi_app.h"
#include "udp.h"
#include "wpl.h"
#include "wlan.h"
#include "pin_mux.h"
#include "mode.h"
#include "log.h"

#define SSID_AP "AP"
#define PASSWORD_AP "12345678"
#define WLAN_CHANNEL 36 // 5170–5190 MHz
#define NETWORK_LABEL "default"

// Prototypes
static void link_status_change_cb(bool link_state);
static void wait_for_ip_address(int is_station_mode);
static void start_ap(void);
static void start_sta(void);

TaskHandle_t g_wifi_task_handle = NULL;

EventGroupHandle_t g_wifi_events;

void wifi_task(void *pvParameters)
{
    wpl_ret_t ret_val;
    log_print("\r\nInitializing Wi-Fi driver...\r\n");
    ret_val = WPL_Init();
    if (ret_val != WPLRET_SUCCESS)
    {
        log_print("WPL_Init() Failed, error: %d\r\n", ret_val);
        while (1);
    }

    log_print("\r\nStarting Wi-Fi driver...\r\n");
    ret_val = WPL_Start(link_status_change_cb);
    if (ret_val != WPLRET_SUCCESS)
    {
        log_print("WPL_Start() Failed, error: %d\r\n", ret_val);
        while (1);
    }


    while(1) {
        // Block here until set_current_app_mode() sends a notification.
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // Clear the connected flag immediately so UDP task stop processing
        xEventGroupClearBits(g_wifi_events, WIFI_EVENT_IP_ACQUIRED);
        
        // Teardown previous AP or STA connection
        WPL_Stop_AP();
        WPL_Leave();

        // Determine app mode
        app_mode_t current_mode = get_app_mode();
        if (current_mode == MODE_UDP_DONGLE_AUDIO) 
        {
            log_print("Starting AP Mode...\r\n");
            start_ap();
        }
        else if (current_mode == MODE_UDP_HEADSET_AUDIO) 
        {
            log_print("Starting STA Mode...\r\n");
            start_sta();
        }
    }
}

static void start_ap(void){
    // Force 20MHz for better SNR/Wall penetration
    if(wlan_uap_set_bandwidth(1) != WPLRET_SUCCESS) {
        log_print("Failed to set AP bandwidth\r\n");
    }

    // Start AP
    wpl_ret_t err = WPLRET_FAIL;
    err = WPL_Start_AP(SSID_AP, PASSWORD_AP, WLAN_CHANNEL);
    if (err == WPLRET_SUCCESS) {
        log_print("SoftAP Started.");
    }
    else {
        log_print("WPL_Start_AP() Start Failed: %d\r\n", err);
    }
      
    // Disable Aggregate MAC Protocol Data Unit (AMPDU) in both directions.
    // Disable TX Aggregation: Send frames immediately, don't batch.
    wlan_uap_ampdu_tx_disable();
    // Disable RX Aggregation: Process incoming frames one-by-one.
    wlan_uap_ampdu_rx_disable();
        
    // Confirm our own IP (Should be 192.168.1.1)
    wait_for_ip_address(0); // 0 = AP Mode
        
    // Set RTS threshold > 2346 (max packet size).
    // Effectively disables RTS/CTS handshake overhead for all packets.
    log_print("Waiting for Headset to connect to apply RTS Optimization...\r\n");
    while (1) {
        // Try to set RTS threshold
        if (wlan_set_uap_rts(2347) == WM_SUCCESS) {
            log_print("Success: RTS Threshold set to 2347 (Disabled).\r\n");
            break;
        }
        
        // If failed, it means no client/STA is connected yet. Wait and retry.
        vTaskDelay(pdMS_TO_TICKS(2000)); 
    }
}

static void start_sta(void){
    wpl_ret_t err = WPLRET_FAIL;
    
    log_print("Adding default Wi-Fi Network...\r\n");
    
    err = WPL_AddNetwork(SSID_AP, PASSWORD_AP, NETWORK_LABEL);
    
    if (err != WPLRET_SUCCESS)
    {
        log_print("WPL_AddNetwork() Failed, error: %d\r\n", (uint32_t)err);
    }
    
    log_print("Device joining the Wi-Fi Network using its STA interface...\r\n");
    
    err = WPLRET_FAIL;
    err = WPL_Join(NETWORK_LABEL);
    
    if (err == WPLRET_SUCCESS)
    {
        log_print("Connected. Optimizing for Latency...\r\n");
        // Disable Power Save (Crucial: Stops AP from buffering data)
        if (wlan_ieeeps_off() != WPLRET_SUCCESS) {
            log_print("Failed to disable Power Save mode\r\n");
        }

        // Disable Aggregate MAC Protocol Data Unit (AMPDU) for both RX and TX.
        // Disable TX Aggregation: Send frames immediately, don't batch.
        wlan_sta_ampdu_tx_disable();
        // Disable RX Aggregation: Process incoming frames one-by-one.
        wlan_sta_ampdu_rx_disable();

        if (wlan_set_roaming(0, 0) != WPLRET_SUCCESS) {
            log_print("Failed to disable roaming\r\n");
        }

        // Set RTS threshold > 2346 (max packet size).
        // Effectively disables RTS/CTS handshake overhead for all packets.
        if(wlan_set_rts(2347) != WPLRET_SUCCESS) {
            log_print("Failed to set RTS threshold\r\n");
        }

        wait_for_ip_address(1); // 1 = STA mode
    }
    else if (err != WPLRET_SUCCESS)
    {
        log_print("WPL_Join() Failed, error: %d\r\n", (uint32_t)err);
    }
}

static void link_status_change_cb(bool link_state)
{
    if (link_state == false)
    {
        log_print("-------- LINK LOST --------\r\n");
    }
    else
    {
        log_print("-------- LINK REESTABLISHED --------\r\n");
    }
}

static void wait_for_ip_address(int is_station_mode)
{
    char ip_str[32];
    wpl_ret_t ret;
    int role_param = is_station_mode ? 1 : 0; // 1=STA, 0=AP

    log_print("Waiting for IP address...\r\n");

    while (1)
    {
        // Try to get the IP
        ret = WPL_GetIP(ip_str, role_param);

        // Check if SUCCESS and if the string is not empty or "0.0.0.0"
        if (ret == WPLRET_SUCCESS && 
            strcmp(ip_str, "0.0.0.0") != 0 && 
            strlen(ip_str) > 0)
        {
            log_print("IP Address Assigned: %s\r\n", ip_str);
            // Signal the IP Acquiration to the UDP task
            xEventGroupSetBits(g_wifi_events, WIFI_EVENT_IP_ACQUIRED);
            break;
        }

        // Wait 500ms before retrying
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
