    
// File: espnow_rx.c
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"


static const char *TAG = "ESPNOW_RX";

// ================== CONFIGURATION ==================
// Set the GPIO pin to toggle on receive
#define GPIO_OUTPUT_PIN     GPIO_NUM_15

// MAC address of the Dongle (TX)
static uint8_t peer_mac_address[] = {0x40, 0x4C, 0xCA, 0x56, 0x26, 0xAC};

// I2S port and GPIOs
#define I2S_NUM         (0)
#define I2S_MCK_IO      23
#define I2S_BCK_IO      20
#define I2S_WS_IO       3
#define I2S_DO_IO       2
#define I2S_DI_IO       5 // (unused for now)

// I2S PCM settings
#define I2S_SAMPLE_RATE     48000   // 48 kHz
#define I2S_CHANNEL_NUM     2       // 2 Channels for stereo audio
#define I2S_BITS_PER_SAMPLE 16      // 16 Bit

// Audio buffer settings
#define BUFFER_DURATION_MS 40 
#define BYTES_PER_SAMPLE (I2S_BITS_PER_SAMPLE / 8)
#define SAMPLES_PER_MS (I2S_SAMPLE_RATE / 1000)
#define AUDIO_BUFFER_SIZE (SAMPLES_PER_MS * BYTES_PER_SAMPLE * I2S_CHANNEL_NUM * BUFFER_DURATION_MS)
#define PLAYBACK_START_THRESHOLD (AUDIO_BUFFER_SIZE / 2) // Buffer fill state threshold

// ===================================================

// Define the same data structure to interpret the packet
typedef struct {
    uint32_t count;         // 2^32 packets are about 348 days of audio playback before overrun happens
    uint8_t  payload[1344]; // 7ms audio @ 48kHz Stereo 16 Bit
} audio_packet_t;


// Global Handles
static i2s_chan_handle_t i2s_tx_handle = NULL;
static RingbufHandle_t audio_ring_buffer = NULL;


// Callback function for when a packet is received
void on_data_received(const esp_now_recv_info_t *recv_info, const uint8_t *incoming_data, int len) {
    // Set GPIO HIGH immediately upon reception
    // gpio_set_level(GPIO_OUTPUT_PIN, 1);
    // gpio_set_level(GPIO_OUTPUT_PIN, 0);

    static uint32_t last_packet_count = UINT32_MAX; // UINT32_MAX indicates no package was previously received.
    static uint32_t packets_lost_total = 0;
    
    if (len != sizeof(audio_packet_t)) {
        ESP_LOGE(TAG, "Received packet of unexpected size from " MACSTR, MAC2STR(recv_info->src_addr));
        return;
    }

    audio_packet_t received_packet;
    memcpy(&received_packet, incoming_data, sizeof(received_packet));

    // Check for packet loss
    if (last_packet_count != UINT32_MAX && received_packet.count > last_packet_count + 1) {
        uint32_t packets_missed = received_packet.count - (last_packet_count + 1);
        packets_lost_total += packets_missed;
        ESP_LOGW(TAG, "Packet loss! Missed %lu packets. Total lost: %lu", packets_missed, packets_lost_total);
        // TODO: Handle packet loss here!
    }
    last_packet_count = received_packet.count;

    // Send received audio data to the ring buffer
    // Note: Using a timeout of 0, so we don't block the callback.
    // If the buffer is full, data is dropped.
    BaseType_t result = xRingbufferSend(audio_ring_buffer, received_packet.payload, sizeof(received_packet.payload), 0);
    if (result != pdTRUE) {
        if(received_packet.count%1000 == 0){
            ESP_LOGE(TAG, "Couldnt write to audio buffer (overflow or timeout). Dropping packet.");
        }
    }
    else{
        if(received_packet.count%1000 == 0){
        UBaseType_t buffered_size = 0;
        vRingbufferGetInfo(audio_ring_buffer, NULL, NULL, NULL, NULL, &buffered_size);
        ESP_LOGI(TAG, "Put %d bytes into buf, current bytes in buf: %lu", sizeof(received_packet.payload), (unsigned long) buffered_size);
        }
    }
    
    // if (received_packet.count%1000 == 0) {
    //         ESP_LOGI(TAG, "len: %d cnt: #%lu src: " MACSTR, len, received_packet.count, MAC2STR(recv_info->src_addr));
    // }
}

static esp_err_t i2s_driver_init(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &i2s_tx_handle, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_BITS_PER_SAMPLE, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCK_IO,
            .bclk = I2S_BCK_IO,
            .ws = I2S_WS_IO,
            .dout = I2S_DO_IO,
            .din = I2S_DI_IO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_tx_handle, &std_cfg));
    return ESP_OK;
}


void i2s_playback_task(void *args) {
    uint8_t *audio_data = NULL;
    size_t item_size = 0;
    bool is_playing = false;

    ESP_LOGI(TAG, "I2S Playback Task Started. Buffer size: %d, Threshold: %d", AUDIO_BUFFER_SIZE, PLAYBACK_START_THRESHOLD);
    
    while(1) {
        // --- STATE 1: PRE-BUFFERING & PRELOADING ---
        if (!is_playing) {
            UBaseType_t buffered_size = 0;
            vRingbufferGetInfo(audio_ring_buffer, NULL, NULL, NULL, NULL, &buffered_size);
            
            if (buffered_size >= PLAYBACK_START_THRESHOLD) {
                ESP_LOGI(TAG, "Buffer threshold reached (%lu bytes). Preloading I2S buffer...", (unsigned long)buffered_size);
                
                // 1. Receive the FIRST chunk of data for preloading
                uint8_t* preload_data = (uint8_t *)xRingbufferReceive(audio_ring_buffer, &item_size, pdMS_TO_TICKS(0));

                if (preload_data != NULL) {
                    size_t bytes_written = 0;

                    // 2. Preload this first chunk into the DMA buffer
                    ESP_ERROR_CHECK(i2s_channel_preload_data(i2s_tx_handle, preload_data, item_size, &bytes_written));
                    
                    // 3. IMPORTANT: Return the buffer item immediately after use
                    vRingbufferReturnItem(audio_ring_buffer, (void *)preload_data);

                    // 4. Now, enable the channel. The clock starts and plays the preloaded data.
                    //ESP_LOGI(TAG, "Preload complete. Starting playback.");
                    ESP_ERROR_CHECK(i2s_channel_enable(i2s_tx_handle));
                    is_playing = true; // Transition to the "playing" state
                } else {
                    // This is unlikely but a good safeguard
                    ESP_LOGW(TAG, "Failed to get data for preloading, will try again.");
                    vTaskDelay(pdMS_TO_TICKS(10));
                }

            } else {
                // Buffer isn't full enough, wait and check again
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }

        // --- STATE 2: PLAYING (NORMAL OPERATION) ---
        if (is_playing) {
            //ESP_LOGI(TAG, "Entering playing state!");
            // Wait for the NEXT chunk of data from the buffer
            audio_data = (uint8_t *)xRingbufferReceive(audio_ring_buffer, &item_size, pdMS_TO_TICKS(100));

            if (audio_data != NULL) {
                // We got data, write it to the (now running) I2S channel
                size_t bytes_written = 0;
                esp_err_t ret = i2s_channel_write(i2s_tx_handle, audio_data, item_size, &bytes_written, portMAX_DELAY);
                
                if (ret != ESP_OK) {
                     ESP_LOGE(TAG, "I2S Write Error: %s", esp_err_to_name(ret));
                }
                
                vRingbufferReturnItem(audio_ring_buffer, (void *)audio_data);

            } else {
                // BUFFER UNDERRUN!
                ESP_LOGW(TAG, "Buffer underrun! Pausing playback.");
                ESP_ERROR_CHECK(i2s_channel_disable(i2s_tx_handle));
                is_playing = false; // Go back to the pre-buffering state
            }
        }
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

    // Initialize I2S
    ESP_LOGI(TAG, "Initializing I2S driver...");
    if (i2s_driver_init() != ESP_OK) {
        ESP_LOGE(TAG, "I2S driver init failed");
        abort();
    }

    // Create the audio ring buffer
    audio_ring_buffer = xRingbufferCreate(AUDIO_BUFFER_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (audio_ring_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to create ring buffer");
        abort();
    }

    // Create the I2S playback task 
    xTaskCreate(i2s_playback_task, "i2s_playback_task", 4096, NULL, 5, NULL);

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

    
    
}

  