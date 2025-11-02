    
// File: espnow_rx.c
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

static const char *TAG = "ESPNOW_RX";

// ================== CONFIGURATION ==================
// Set the GPIO pin to toggle on receive
#define GPIO_OUTPUT_PIN     GPIO_NUM_15

// Add the MAC address of the Dongle (TX) device here!
static uint8_t peer_mac_address[] = {0x40, 0x4C, 0xCA, 0x56, 0x26, 0xAC};
// ===================================================

// Define the same data structure to interpret the packet
typedef struct {
    uint32_t count;
    uint8_t  payload[1344]; // 7ms audio 48kHz Stereo 16 Bit
} audio_packet_t;


// Callback function for when a packet is received
void on_data_received(const esp_now_recv_info_t *recv_info, const uint8_t *incoming_data, int len) {
    // Set GPIO HIGH immediately upon reception
    gpio_set_level(GPIO_OUTPUT_PIN, 1);
    gpio_set_level(GPIO_OUTPUT_PIN, 0);

    static int64_t last_packet_count = -1; 
    static uint32_t packets_lost_total = 0;
    
    // Check if the packet is the correct size
    if (len == sizeof(audio_packet_t)) {
        audio_packet_t received_packet;
        memcpy(&received_packet, incoming_data, sizeof(received_packet));

        // This is the first packet we've ever received.
        if (last_packet_count == -1) {
            last_packet_count = received_packet.count;
        } 

        // Check for a break in the sequence.
        else if (received_packet.count > last_packet_count + 1) {
            uint32_t packets_missed = received_packet.count - (last_packet_count + 1);
            packets_lost_total += packets_missed;
            ESP_LOGW(TAG, "Packet loss! Missed %lu packets. Total lost: %lu", packets_missed, packets_lost_total);
        }
        // IMPORTANT: Only update our state with the latest valid count.
        last_packet_count = received_packet.count;

        if (received_packet.count%1000 == 0) {
            ESP_LOGI(TAG, "len: %d cnt: #%lu src: " MACSTR, len, received_packet.count, MAC2STR(recv_info->src_addr));
        }

    } else {
        ESP_LOGE(TAG, "Received packet of unexpected size from " MACSTR, MAC2STR(recv_info->src_addr));
    }
}

void app_main(void) {
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize networking stack
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());


    // Initialize GPIO
    gpio_reset_pin(GPIO_OUTPUT_PIN);
    gpio_set_direction(GPIO_OUTPUT_PIN, GPIO_MODE_OUTPUT);

    // Initialize ESP-NOW
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_data_received));

    // Add the sender (Dongle) as a peer
    esp_now_peer_info_t peer_info = {};
    memcpy(peer_info.peer_addr, peer_mac_address, 6);
    peer_info.channel = 0;
    peer_info.ifidx = WIFI_IF_STA;
    peer_info.encrypt = false; // Set to true if you add a key
    ESP_ERROR_CHECK(esp_now_add_peer(&peer_info));

    // Configure the rate for that peer
    ESP_LOGI(TAG, "Setting peer rate config...");
    esp_now_rate_config_t rate_config = {
                .phymode = WIFI_PHY_MODE_HE20,    
                .rate = WIFI_PHY_RATE_MCS9_SGI,     
                .ersu = false,                     
                .dcm = false                       
    };
    ESP_ERROR_CHECK(esp_now_set_peer_rate_config(peer_mac_address, &rate_config));

    ESP_LOGI(TAG, "Setup complete. Waiting for packets...");
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

  