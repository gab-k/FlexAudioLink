#include "wifi_app.h"
#include "udp.h"
#include "wpl.h"
#include "wlan.h"
#include "pin_mux.h"
#include "mode.h"
#include "log.h"
#include "wifi-internal.h"

#define SSID_AP "AP"
#define PASSWORD_AP "12345678"
#define WLAN_CHANNEL 48 
#define NETWORK_LABEL "default"
#define wifi_drv_task_priority (configMAX_PRIORITIES - 2)
#define IMU_TASK_PRIORITY (configMAX_PRIORITIES - 3)
#define wifi_drv_tx_task_priority (configMAX_PRIORITIES - 4)
#define wifi_scan_task_priority (configMAX_PRIORITIES - 7)
#define wifi_powersave_task_priority (configMAX_PRIORITIES - 7)


// Prototypes
static void link_status_change_cb(bool link_state);
static void wait_for_ip_address(int is_station_mode);
static void start_ap(void);
static void start_sta(void);
//static void configure_reliable_rates(int bss_type);
static void wifi_print_sta_diagnostics(void);
static void wifi_print_uap_diagnostics(void);
static void set_wifi_task_priorities(void);

// Access the global wifi structure
extern wm_wifi_t wm_wifi;

TaskHandle_t g_wifi_init_task_handle = NULL;

EventGroupHandle_t g_wifi_events;

void wifi_init_task(void *pvParameters)
{
    wpl_ret_t ret_val;
    PRINTF("\r\nInitializing Wi-Fi driver...\r\n");
    ret_val = WPL_Init();
    if (ret_val != WPLRET_SUCCESS)
    {
        PRINTF("WPL_Init() Failed, error: %d\r\n", ret_val);
        while (1);
    }

    PRINTF("\r\nStarting Wi-Fi driver...\r\n");
    ret_val = WPL_Start(link_status_change_cb);
    if (ret_val != WPLRET_SUCCESS)
    {
        PRINTF("WPL_Start() Failed, error: %d\r\n", ret_val);
        while (1);
    }
    
    // int ret = wlan_set_country_code("EUI");
    // if (ret != WM_SUCCESS)
    // {
    //     PRINTF("wlan_set_country_code() Failed, error: %d\r\n", ret);
    //     while (1);
    // }

    while(1) {
        // Block here until set_current_app_mode() sends a notification.
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // Clear the connected flag immediately so UDP task stop processing
        xEventGroupClearBits(g_wifi_events, WIFI_INIT_DONE);
        
        // Teardown previous AP or STA connection
        WPL_Stop_AP();
        WPL_Leave();

        // Determine app mode
        app_mode_t current_mode = get_app_mode();
        if (current_mode == MODE_UDP_DONGLE_AUDIO) 
        {
            PRINTF("Starting AP Mode...\r\n");
            start_ap();
        }
        else if (current_mode == MODE_UDP_HEADSET_AUDIO) 
        {
            PRINTF("Starting STA Mode...\r\n");
            start_sta();
        }
        set_wifi_task_priorities();
    }
}

static void set_wifi_task_priorities(void) {
    osa_status_t status;
    
    status = KOSA_StatusError;
    status = OSA_TaskSetPriority(wm_wifi.wifi_drv_task_Handle, PRIORITY_RTOS_TO_OSA(wifi_drv_task_priority));
    if (status == KOSA_StatusSuccess) {
        PRINTF("Promoted wifi_drv_task to Prio %d\r\n", wifi_drv_task_priority);
    }
    else {
        PRINTF("Error setting wifi_drv_task Priority!\r\n");
    }

    status = KOSA_StatusError;
    status = OSA_TaskSetPriority(wm_wifi.wifi_drv_tx_task_Handle, PRIORITY_RTOS_TO_OSA(wifi_drv_tx_task_priority));
    if (status == KOSA_StatusSuccess) {
        PRINTF("Promoted wifi_drv_tx_task to Prio %d\r\n", wifi_drv_tx_task_priority);
    }
    else {
        PRINTF("Error setting wifi_drv_tx_task Priority!\r\n");
    }

    status = KOSA_StatusError;
    status = OSA_TaskSetPriority(wm_wifi.wifi_scan_task_Handle, PRIORITY_RTOS_TO_OSA(wifi_scan_task_priority));
    if (status == KOSA_StatusSuccess) {
        PRINTF("Promoted wifi_scan_task to Prio %d\r\n", wifi_scan_task_priority);
    }
    else {
        PRINTF("Error setting wifi_scan_task Priority!\r\n");
    }

    status = KOSA_StatusError;
    status = OSA_TaskSetPriority(wm_wifi.wifi_powersave_task_Handle, PRIORITY_RTOS_TO_OSA(wifi_powersave_task_priority));
    if (status == KOSA_StatusSuccess) {
        PRINTF("Promoted wifi_powersave_task to Prio %d\r\n", wifi_powersave_task_priority);
    }
    else {
        PRINTF("Error setting wifi_powersave_task Priority!\r\n");
    }
    // Signal the wifi init done to the UDP task
    xEventGroupSetBits(g_wifi_events, WIFI_INIT_DONE);
}

static void start_ap(void){
    // Force 20MHz for better SNR/Wall penetration
    if(wlan_uap_set_bandwidth(1) != WPLRET_SUCCESS) {
        PRINTF("Failed to set AP bandwidth\r\n");
    }

    wlan_ed_mac_ctrl_t ed_ctrl;
    memset(&ed_ctrl, 0, sizeof(ed_ctrl));
    // Disable Energy Detect Adaptivity for 5GHz
    ed_ctrl.ed_ctrl_5g = 0; 
    ed_ctrl.ed_offset_5g = 0; // Default offset
    if (wlan_set_ed_mac_mode(ed_ctrl) != WM_SUCCESS) {
        PRINTF("Failed to disable EDMAC (Politeness Mode)\r\n");
    }

    wlan_set_11d_state(1, 0); // 1 = uAP, 0 = Disable
    wlan_set_11d_state(0, 0);

    wlan_uap_ampdu_tx_disable();
    wlan_uap_ampdu_rx_disable();

    wlan_set_wmm_uapsd(0);

    // Disable Power Save (Crucial: Stops AP from buffering data)
    if (wlan_ieeeps_off() != WPLRET_SUCCESS) {
        PRINTF("Failed to disable Power Save mode\r\n");
    }

    // Disable Deep Sleep Power Save (THE SMOKING GUN)
    if (wlan_deepsleepps_off() != WPLRET_SUCCESS) {
        PRINTF("Failed to disable Deep Sleep\r\n");
    }

    // Start AP
    wpl_ret_t err = WPLRET_FAIL;
    err = WPL_Start_AP(SSID_AP, PASSWORD_AP, WLAN_CHANNEL);
    if (err == WPLRET_SUCCESS) {
        PRINTF("SoftAP Started.");
    }
    else {
        PRINTF("WPL_Start_AP() Start Failed: %d\r\n", err);
    }

    //configure_reliable_rates(BSS_TYPE_UAP);
      
    // Disable Aggregate MAC Protocol Data Unit (AMPDU) in both directions.
    // Disable TX Aggregation: Send frames immediately, don't batch.
    wlan_uap_ampdu_tx_disable();
    // Disable RX Aggregation: Process incoming frames one-by-one.
    wlan_uap_ampdu_rx_disable();
        
    // Confirm our own IP (Should be 192.168.1.1)
    wait_for_ip_address(0); // 0 = AP Mode
        
    wlan_set_roaming(0, 0);

    // Set RTS threshold > 2346 (max packet size).
    // Effectively disables RTS/CTS handshake overhead for all packets.
    PRINTF("Waiting for Headset to connect to apply RTS Optimization...\r\n");
    while (1) {
        // Try to set RTS threshold
        if (wlan_set_uap_rts(2347) == WM_SUCCESS) {
            PRINTF("Success: RTS Threshold set to 2347 (Disabled).\r\n");
            break;
        }
        
        // If failed, it means no client/STA is connected yet. Wait and retry.
        vTaskDelay(pdMS_TO_TICKS(2000)); 
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
    wifi_print_uap_diagnostics();
}

static void start_sta(void){
    wpl_ret_t err = WPLRET_FAIL;
    
    PRINTF("Adding default Wi-Fi Network...\r\n");
    
    err = WPL_AddNetwork(SSID_AP, PASSWORD_AP, NETWORK_LABEL);
    
    if (err != WPLRET_SUCCESS)
    {
        PRINTF("WPL_AddNetwork() Failed, error: %d\r\n", (uint32_t)err);
    }
    
    PRINTF("Device joining the Wi-Fi Network using its STA interface...\r\n");

    // Disable Aggregate MAC Protocol Data Unit (AMPDU) for both RX and TX.
    // Disable TX Aggregation: Send frames immediately, don't batch.
    wlan_sta_ampdu_tx_disable();
    // Disable RX Aggregation: Process incoming frames one-by-one.
    wlan_sta_ampdu_rx_disable();

    // Disable WMM Power Save
    if (wlan_set_wmm_uapsd(0) != WPLRET_SUCCESS){
        PRINTF("Failed to Disable WMM Power Save\r\n");
    }

    // Disable 802.11d (0 = STA, 0 = Disable)
    if (wlan_set_11d_state(0, 0) != WPLRET_SUCCESS){
        PRINTF("Failed to disable 802.11d\r\n");
    }

    // Disable 11k Measurements
    //if (wlan_host_11k_cfg(0) != WPLRET_SUCCESS){
    //    PRINTF("Failed to Disable 11k Measurements\r\n");
    //}

    wlan_ed_mac_ctrl_t ed_ctrl;
    memset(&ed_ctrl, 0, sizeof(ed_ctrl));
    // Disable Energy Detect Adaptivity for 5GHz
    ed_ctrl.ed_ctrl_5g = 0; 
    ed_ctrl.ed_offset_5g = 0; // Default offset
    if (wlan_set_ed_mac_mode(ed_ctrl) != WM_SUCCESS) {
        PRINTF("Failed to disable EDMAC (Politeness Mode)\r\n");
    }

    err = WPLRET_FAIL;
    err = WPL_Join(NETWORK_LABEL);
    
    if (err == WPLRET_SUCCESS)
    {
        PRINTF("Connected. Optimizing for Latency...\r\n");
        // configure_reliable_rates(BSS_TYPE_STA);
        // Disable Power Save (Crucial: Stops AP from buffering data)
        if (wlan_ieeeps_off() != WPLRET_SUCCESS) {
            PRINTF("Failed to disable Power Save mode\r\n");
        }

        // Disable Deep Sleep Power Save (THE SMOKING GUN)
        if (wlan_deepsleepps_off() != WPLRET_SUCCESS) {
            PRINTF("Failed to disable Deep Sleep\r\n");
        }

        // Disable Aggregate MAC Protocol Data Unit (AMPDU) for both RX and TX.
        // Disable TX Aggregation: Send frames immediately, don't batch.
        wlan_sta_ampdu_tx_disable();
        // Disable RX Aggregation: Process incoming frames one-by-one.
        wlan_sta_ampdu_rx_disable();

        if (wlan_set_roaming(0, 0) != WPLRET_SUCCESS) {
            PRINTF("Failed to disable roaming\r\n");
        }

        wait_for_ip_address(1); // 1 = STA mode

        // Set RTS threshold > 2346 (max packet size).
        // Effectively disables RTS/CTS handshake overhead for all packets.
        if(wlan_set_rts(2347) != WPLRET_SUCCESS) {
            PRINTF("Failed to set RTS threshold\r\n");
        }
    }
    else if (err != WPLRET_SUCCESS)
    {
        PRINTF("WPL_Join() Failed, error: %d\r\n", (uint32_t)err);
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
    wifi_print_sta_diagnostics();
}

// static void configure_reliable_rates(int bss_type)
// {
//     wifi_ds_rate rate_config;
    
//     // Clear memory to avoid garbage
//     memset(&rate_config, 0, sizeof(wifi_ds_rate));

//     // 1. Set the Command Type
//     rate_config.sub_command = WIFI_DS_RATE_CFG;

//     // 2. Configure the Parameters
//     // rate_format: 1 = HT (High Throughput / 802.11n)
//     //              0 = Legacy (11a/b/g), 2 = VHT (11ac), 3 = HE (11ax)
//     rate_config.param.rate_cfg.rate_format = 0; // Use HT (11n) for 2.4/5GHz reliability

//     // 3. Set the MCS Index
//     // Lock to MCS 3 or 4. 
//     // MCS 3 = 16-QAM, 1/2 Rate (Very Robust, ~26Mbps)
//     // MCS 4 = 16-QAM, 3/4 Rate (Robust, ~39Mbps)
//     rate_config.param.rate_cfg.rate_index = 0; 

//     // 4. Rate Setting (Optional/Advanced)
//     // This is usually for setting preamble type or bandwidth (20MHz vs 40MHz)
//     // Leaving it 0 usually defaults to "Don't Change" or "Auto".
//     // If you need to force 20MHz bandwidth here too:
//     // rate_config.param.rate_cfg.rate_setting = ... (See txrate_setting struct in header)

//     PRINTF("Locking WiFi Rate to MCS %d (HT)...\r\n", rate_config.param.rate_cfg.rate_index);

//     // 5. Apply the Setting
//     // param 1: The struct we just built
//     // param 2: The interface (0 = STA, 1 = uAP). 
//     // You likely want to set this for BOTH if possible, or at least the sender (Dongle).
    
//     // Apply to AP (if this is the Dongle)
//     int ret = wlan_set_txratecfg(rate_config, bss_type);
//     if (ret != WM_SUCCESS) {
//         PRINTF("Failed to set Rate: %d\r\n", ret);
//     }
// }

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
            break;
        }

        // Wait 500ms before retrying
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}



static void wifi_print_sta_diagnostics(void)
{
    PRINTF("\r\n--- LIVE WI-FI FIRMWARE STATE ---\r\n");

    // 1. Check Power Save Status (The #1 Suspect)
    bool is_ps_enabled = wlan_is_power_save_enabled();
    PRINTF("Power Save Enabled : %s\r\n", is_ps_enabled ? "YES (WARNING!)" : "NO (Good)");

    // Get the exact power save mode
    enum wlan_ps_mode ps_mode;
    if (wlan_get_ps_mode(&ps_mode) == WLAN_ERROR_NONE) {
        PRINTF("Power Save Mode    : ");
        switch(ps_mode) {
            case WLAN_ACTIVE: PRINTF("WLAN_ACTIVE\r\n"); break;
            case WLAN_IEEE: PRINTF("WLAN_IEEE\r\n"); break;
            case WLAN_DEEP_SLEEP: PRINTF("WLAN_DEEP_SLEEP\r\n"); break;
            case WLAN_IEEE_DEEP_SLEEP: PRINTF("WLAN_IEEE_DEEP_SLEEP\r\n"); break;
            #if CONFIG_WNM_PS
            case WLAN_WNM: PRINTF("WLAN_WNM\r\n"); break;
            case WLAN_WNM_DEEP_SLEEP: PRINTF("WLAN_WNM_DEEP_SLEEP\r\n"); break;
            #endif
            default: PRINTF("UNKNOWN (%d)\r\n", ps_mode); break;
        }
    }

    // 2. Check Roaming Status
    #if CONFIG_ROAMING
    int roaming_status = wlan_get_roaming_status();
    PRINTF("Roaming Enabled    : %s\r\n", roaming_status ? "YES (WARNING!)" : "NO (Good)");
    #endif

    // 3. Check 802.11d (Regulatory Domain Scanning)
    bool is_11d_enabled = wlan_get_11d_enable_status();
    PRINTF("802.11d Enabled    : %s\r\n", is_11d_enabled ? "YES (Might cause scans)" : "NO (Good)");

    // 4. Check 802.11k (Neighbor Reports / Background Scans)
    #if CONFIG_11K
    bool is_11k_enabled = wlan_get_host_11k_status();
    PRINTF("802.11k Enabled    : %s\r\n", is_11k_enabled ? "YES (Might cause scans)" : "NO (Good)");
    #endif

    // 5. Current Signal Strength (Just to make sure you aren't actually losing RF)
    short rssi;
    int snr;
    if (wlan_get_current_signal_strength(&rssi, &snr) == WLAN_ERROR_NONE) {
        PRINTF("Link Quality       : RSSI %d dBm | SNR %d dB\r\n", rssi, snr);
    }

    PRINTF("---------------------------------\r\n");
}


static void wifi_print_uap_diagnostics(void)
{
    PRINTF("\r\n--- LIVE uAP (DONGLE) FIRMWARE STATE ---\r\n");

    bool is_ps_enabled = wlan_is_power_save_enabled();
    PRINTF("Power Save Enabled : %s\r\n", is_ps_enabled ? "YES (WARNING!)" : "NO (Good)");

    // Get the exact power save mode
    enum wlan_ps_mode ps_mode;
    if (wlan_get_ps_mode(&ps_mode) == WLAN_ERROR_NONE) {
        PRINTF("Power Save Mode    : ");
        switch(ps_mode) {
            case WLAN_ACTIVE: PRINTF("WLAN_ACTIVE\r\n"); break;
            case WLAN_IEEE: PRINTF("WLAN_IEEE\r\n"); break;
            case WLAN_DEEP_SLEEP: PRINTF("WLAN_DEEP_SLEEP\r\n"); break;
            case WLAN_IEEE_DEEP_SLEEP: PRINTF("WLAN_IEEE_DEEP_SLEEP\r\n"); break;
            #if CONFIG_WNM_PS
            case WLAN_WNM: PRINTF("WLAN_WNM\r\n"); break;
            case WLAN_WNM_DEEP_SLEEP: PRINTF("WLAN_WNM_DEEP_SLEEP\r\n"); break;
            #endif
            default: PRINTF("UNKNOWN (%d)\r\n", ps_mode); break;
        }
    }

    // 2. Check Roaming Status
    #if CONFIG_ROAMING
    int roaming_status = wlan_get_roaming_status();
    PRINTF("Roaming Enabled    : %s\r\n", roaming_status ? "YES (WARNING!)" : "NO (Good)");
    #endif

    // 4. Check 802.11k (Neighbor Reports / Background Scans)
    #if CONFIG_11K
    bool is_11k_enabled = wlan_get_host_11k_status();
    PRINTF("802.11k Enabled    : %s\r\n", is_11k_enabled ? "YES (Might cause scans)" : "NO (Good)");
    #endif

    // 1. Check Channel and Bandwidth
    int channel = 0;
    if (wlan_get_uap_channel(&channel) == WLAN_ERROR_NONE) {
        PRINTF("uAP Channel        : %d\r\n", channel);
    }

    uint8_t bandwidth = 0;
    if (wlan_uap_get_bandwidth(&bandwidth) == WLAN_ERROR_NONE) {
        PRINTF("uAP Bandwidth      : %s\r\n", 
               (bandwidth == BANDWIDTH_20MHZ) ? "20 MHz" : 
               (bandwidth == BANDWIDTH_40MHZ) ? "40 MHz" : "80 MHz");
    }

    // 2. Check 802.11d Status
    bool is_11d = wlan_get_11d_enable_status();
    PRINTF("uAP 802.11d        : %s\r\n", is_11d ? "YES (Bad)" : "NO (Good)");

    // 3. WMM UAPSD (Should be Disabled)
    uint8_t uapsd = wlan_is_wmm_uapsd_enabled();
    PRINTF("uAP WMM UAPSD      : %s\r\n", uapsd ? "YES (Bad for Latency)" : "NO (Good)");

    PRINTF("----------------------------------------\r\n");
}