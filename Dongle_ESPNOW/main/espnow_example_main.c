// File: espnow_tx.c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

static const char *TAG = "ESPNOW_TX";

// ================== CONFIGURATION ==================
// Set the GPIO pin to toggle on send
#define GPIO_OUTPUT_PIN     GPIO_NUM_15

// Replace this with the MAC address of your Headset (RX) device
static uint8_t peer_mac_address[] = {0x40, 0x4C, 0xCA, 0x4F, 0x3E, 0x18};
// ===================================================

// Define a simple data structure for the packet
typedef struct {
    uint32_t count;
    uint8_t  payload[1344]; // 7ms audio 48kHz Stereo 16 Bit
} audio_packet_t;

// Callback function for when a packet is sent
void on_data_sent(const esp_now_send_info_t *tx_info, esp_now_send_status_t status) {
    // Set the GPIO low AFTER the transmission is confirmed
    if (status != ESP_NOW_SEND_SUCCESS) {
        ESP_LOGE(TAG, "Send failed");
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
    gpio_set_level(GPIO_OUTPUT_PIN, 0);
    
    // Initialize ESP-NOW
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_data_sent));
    
    // Add the peer (the Headset/Receiver)
    esp_now_peer_info_t peer_info = {};
    memcpy(peer_info.peer_addr, peer_mac_address, 6);
    peer_info.channel = 0; // 0 means use the current channel
    peer_info.ifidx = WIFI_IF_STA;
    peer_info.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer_info));
    
    esp_now_rate_config_t rate_config = {
                .phymode = WIFI_PHY_MODE_HE20,    
                .rate = WIFI_PHY_RATE_MCS9_SGI,     
                .ersu = false,                     
                .dcm = false                       
            };
    ESP_ERROR_CHECK(esp_now_set_peer_rate_config(peer_mac_address, &rate_config));

    ESP_LOGI(TAG, "Setup complete. Starting to send packets...");

    audio_packet_t packet_to_send;
    packet_to_send.count = 0;

    while (1) {
        // Set GPIO HIGH just before sending
        gpio_set_level(GPIO_OUTPUT_PIN, 1);
        gpio_set_level(GPIO_OUTPUT_PIN, 0);

        esp_err_t result = esp_now_send(peer_mac_address, (uint8_t *)&packet_to_send, sizeof(packet_to_send));
        
        if (result == ESP_OK) {
            if(packet_to_send.count%1000 == 0)
                ESP_LOGI(TAG, "Sent packet #%lu", packet_to_send.count);
        } else {
            ESP_LOGE(TAG, "Error sending packet: %s", esp_err_to_name(result));
        }

        packet_to_send.count++;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}