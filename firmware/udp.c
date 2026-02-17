#include "udp.h"
#include "lwip/sockets.h"
#include "stdio.h"
#include "pin_mux.h"
#include "fsl_gpio.h"
#include "wifi_app.h"
#include "log.h"
#include "mode.h"

#define UDP_AUDIO_PORT 5000
#define SOCKET_RETRY_DELAY_MS 1000
#define IP_AP "192.168.1.1"     // Dongle IP (Correct)
#define IP_STA "192.168.1.2"  // Broadcast Address for 192.168.1.x subnet

// Latency Tuning:
// 48kHz * 2 channels * 2 bytes = 192 bytes per millisecond.
// 512 bytes = 2.66 ms
// 480 bytes = 2.5 ms
// 240 bytes = 1.25 ms
// 192 bytes = 1 ms
#define UDP_PACKET_SIZE  192


// TODO: Revise all the buffer sizes in this file and #define them
// TODO: Consider making buffer sizes configurable via cli.
#define UDP_SPK_BUF_SIZE   8192
#define UDP_MIC_BUF_SIZE   8192

TaskHandle_t g_udp_task_handle = NULL;

static volatile bool g_feedback_pending = false;
static volatile uint32_t g_feedback_value = 0;

uint16_t tx_udp_packet_counter = 0;
uint16_t rx_udp_packet_counter = 0;

TU_ATTR_ALIGNED(4) static uint8_t udp_spk_buf[UDP_SPK_BUF_SIZE];
TU_ATTR_ALIGNED(4) static uint8_t udp_mic_buf[UDP_MIC_BUF_SIZE];

static tu_fifo_t udp_spk_ff;
static tu_fifo_t udp_mic_ff;

// UDP Header structure, data follows this header!
// For audio packets, raw PCM data is appended after this header.
// For feedback packets, the data following the header is a 16.16 fixed-point value.
typedef struct {
    // [Byte 0] Message Type
    // 0 = Audio Stream (Payload is raw PCM appended after this header)
    // 1 = Feedback (Payload is 16.16 Drift Value)
    // 2 = Command (Payload is Parameter, Flags determine action)
    uint8_t type;
    
    // [Byte 1] Flags / Sub-Type
    // For Audio:  [Bit 0: End of Frame]
    // For Cmd:    [0: Mute], [1: Volume], [2: Play/Pause]
    uint8_t flags;
    
    // [Bytes 2-3] Sequence Number
    // Used to detect packet loss, out-of-order packets.
    uint16_t sequence;
} udp_header_t;

typedef enum {
    UDP_DATATYPE_SPEAKER_AUDIO = 0, // Downlink: Dongle -> Headset
    UDP_DATATYPE_MIC_AUDIO     = 1, // Uplink:   Headset -> Dongle
    UDP_DATATYPE_FEEDBACK      = 2, // Audio rate matching (clock drift compensation)
    UDP_DATATYPE_COMMAND       = 3, // Mute, Vol, Pairing
} udp_datatype_t;


static void udp_process_rx(uint8_t *buffer, int len, app_mode_t mode);
static int udp_process_tx(uint8_t *buffer, app_mode_t mode);


void udp_audio_ff_init(void)
{
    // Glue the Speaker FIFO to the Speaker Buffer
    // true = Overwritable (Circular Mode)
    tu_fifo_config(&udp_spk_ff, udp_spk_buf, UDP_SPK_BUF_SIZE, true);

    // Glue the Mic FIFO to the Mic Buffer
    tu_fifo_config(&udp_mic_ff, udp_mic_buf, UDP_MIC_BUF_SIZE, true);
}    

tu_fifo_t* udp_get_spk_fifo(void) {
    return &udp_spk_ff;
}    

tu_fifo_t* udp_get_mic_fifo(void) {
    return &udp_mic_ff;
}    


void udp_task(void *pvParameters)
{
    int sock = -1;
    struct sockaddr_in my_addr, dest_addr;
    uint8_t rx_buffer[1500];
    uint8_t tx_buffer[1500];

    // Cache variables to track state changes
    app_mode_t current_mode;
    app_mode_t cached_mode = MODE_IDLE; 

    // Pre-calculate IP addresses as integers (Avoids string parsing in the loop)
    const uint32_t ip_addr_ap  = inet_addr(IP_AP);
    const uint32_t ip_addr_sta = inet_addr(IP_STA);

    // Setup destination struct
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(UDP_AUDIO_PORT);

    // Setup own address struct
    memset(&my_addr, 0, sizeof(my_addr));
    my_addr.sin_family = AF_INET;
    my_addr.sin_port = htons(UDP_AUDIO_PORT);
    my_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    for (;;)
    {
        // Wait for IP address to be acquired.
        xEventGroupWaitBits(
            g_wifi_events,           // The Event Group
            WIFI_EVENT_IP_ACQUIRED,  // The Bit to wait for
            pdFALSE,                 // Don't clear the bit on exit (keep it set for others)
            pdTRUE,                  // Wait for the bit to be set
            portMAX_DELAY            // Wait forever
        );

        // Socket Setup & Callback Registration
        if (sock < 0) {
            sock = socket(AF_INET, SOCK_DGRAM, 0);
            
            // Listen to UDP_AUDIO_PORT
            int ret_val = bind(sock, (struct sockaddr *)&my_addr, sizeof(my_addr));
            if (ret_val != 0) {
                int error_code = 0;
                socklen_t len = sizeof(error_code);
                getsockopt(sock, SOL_SOCKET, SO_ERROR, &error_code, &len);
                PRINTF("bind() failed. Return: %d, Actual Error: %d\r\n", ret_val, error_code);
                close(sock);
                sock = -1;
                PRINTF("Retrying in %d ms\r\n", SOCKET_RETRY_DELAY_MS);
                vTaskDelay(pdMS_TO_TICKS(SOCKET_RETRY_DELAY_MS));
                continue;
            }
            
            // Set IP Type of Service to EF (Expedited Forwarding) or Voice
            int tos = 0xE0; // Critical/Voice
            setsockopt(sock, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));

            // Set receive timeout to 1 ms.
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 1000; // 1000us = 1ms
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            
            // Ensure Blocking Mode (Default, but good to be explicit vs previous code)
            int flags = fcntl(sock, F_GETFL, 0);
            fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);

            // Force address update 
            cached_mode = MODE_IDLE; 
        }

        // Configure Target IP based on our Role
        // Only update dest_addr if the mode has changed!
        current_mode = get_app_mode();
        if (current_mode != cached_mode) {
            if (current_mode == MODE_UDP_DONGLE_AUDIO) {
                dest_addr.sin_addr.s_addr = ip_addr_sta;
            } else {
                dest_addr.sin_addr.s_addr = ip_addr_ap;
            }
            cached_mode = current_mode;
        }

        // ---------------------------------------------------------
        // RX and TX Processing Loop
        // ---------------------------------------------------------
        
        // 1. RX Step (Blocking, but wakes on data)
        // If data arrives at 0.3ms, this returns at 0.3ms. 
        // If no data, it returns at 1.0ms.
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0, NULL, NULL);
        if (len > 0) {
            udp_process_rx(rx_buffer, len, current_mode);
            
            // Optimization: If we just got a packet, check for MORE immediately
            // without waiting for the timeout, to drain bursts.
            int flags = fcntl(sock, F_GETFL, 0);
            fcntl(sock, F_SETFL, flags | O_NONBLOCK); // Temp switch to Non-Blocking
            while((len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0, NULL, NULL)) > 0) {
                 udp_process_rx(rx_buffer, len, current_mode);
            }
            fcntl(sock, F_SETFL, flags & ~O_NONBLOCK); // Switch back to Blocking
        }

        // 2. TX Step (Polled)
        // This runs at least once every 1ms.
        // Since USB audio chunks (192-240 bytes) cover ~1ms of time, checking
        // every 1ms is sufficient to keep the pipe full.
        int tx_len = udp_process_tx(tx_buffer, current_mode);
        if (tx_len > 0) {
            sendto(sock, tx_buffer, tx_len, 0, (struct sockaddr *) &dest_addr, sizeof(dest_addr));
        }
    }
}

// ---------------------------------------------------------
// RX PROCESSOR: Validates and Routes Incoming Data
// ---------------------------------------------------------
static void udp_process_rx(uint8_t *buffer, int len, app_mode_t mode)
{
    if (len < sizeof(udp_header_t)) 
    {
        PRINTF("WARN: Received packet too small: %d bytes\r\n", len);
        return;
    }

    udp_header_t *p_hdr = (udp_header_t*)buffer;
    uint8_t *p_payload = &buffer[sizeof(udp_header_t)];
    int payload_len = len - sizeof(udp_header_t);

    switch (p_hdr->type)
    {
    case UDP_DATATYPE_SPEAKER_AUDIO:
        // Only HEADSET should receive Speaker Audio (from Dongle)
        if (mode == MODE_UDP_HEADSET_AUDIO) {
            tu_fifo_write_n(&udp_spk_ff, p_payload, payload_len);
            if (p_hdr->sequence != rx_udp_packet_counter) {
                PRINTF("Packet Loss Detected! Expected Seq: %d, Got: %d\r\n", rx_udp_packet_counter, p_hdr->sequence);
            }
            rx_udp_packet_counter = p_hdr->sequence + 1;
        } else {
            PRINTF("ERROR: Received speaker audio when not in headset mode!\r\n");
            configASSERT(false);
        }
        break;

    case UDP_DATATYPE_MIC_AUDIO:
        // Only DONGLE should receive Mic Audio (from Headset)
        if (mode == MODE_UDP_DONGLE_AUDIO) {
            tu_fifo_write_n(&udp_mic_ff, p_payload, payload_len);
        } else {
            PRINTF("ERROR: Received mic audio when not in dongle mode!\r\n");
            configASSERT(false);
        }
        break;

    case UDP_DATATYPE_FEEDBACK:
        // Only DONGLE should receive Feedback (to sync with USB Host)
        if (mode == MODE_UDP_DONGLE_AUDIO) {
            uint32_t feedback_val;
            memcpy(&feedback_val, p_payload, sizeof(uint32_t));
            tud_audio_fb_set(feedback_val);
        } else {
            // A Headset receiving feedback implies a logic error on the sender
            PRINTF("ERROR: Received Feedback packet when not in dongle mode!\r\n");
            configASSERT(false);
        }
        break;

    case UDP_DATATYPE_COMMAND:
        // TODO: Implement command processing
        // Both might handle commands, or specific ones
        // process_command(p_hdr, p_payload);
        break;

    default:
        PRINTF("Unknown UDP Type: %d\r\n", p_hdr->type);
        configASSERT(false);
        break;
    }
}

// ---------------------------------------------------------
// TX PROCESSOR: Checks Sources and Sends Data
// ---------------------------------------------------------
static int udp_process_tx(uint8_t *buffer, app_mode_t mode)
{
    udp_header_t *p_hdr = (udp_header_t*)buffer;
    uint8_t *p_payload = &buffer[sizeof(udp_header_t)];
    int tx_len = 0;

    if (mode == MODE_UDP_DONGLE_AUDIO)
    {
        // Check if there is USB Audio available for sending to headset.
        uint16_t available = tud_audio_available();
        if (available >= UDP_PACKET_SIZE) 
        {
            p_hdr->type = UDP_DATATYPE_SPEAKER_AUDIO;
            p_hdr->sequence = tx_udp_packet_counter++;
            uint16_t read = tud_audio_read(p_payload, UDP_PACKET_SIZE);
            tx_len = sizeof(udp_header_t) + read;
        }
    }
    else if (mode == MODE_UDP_HEADSET_AUDIO)
    {
        // TODO: Implement Uplink Mic Audio
        if (g_feedback_pending)
        {
            // Enter critical section to ensure we don't clear flag while writing new one
            taskENTER_CRITICAL();
            uint32_t val_to_send = g_feedback_value;
            g_feedback_pending = false; // Clear the mailbox
            taskEXIT_CRITICAL();

            p_hdr->type = UDP_DATATYPE_FEEDBACK;
            p_hdr->sequence = tx_udp_packet_counter++;
            p_hdr->flags = 0;

            // Copy the 4-byte 16.16 value into payload
            memcpy(p_payload, &val_to_send, sizeof(uint32_t));
            
            tx_len = sizeof(udp_header_t) + sizeof(uint32_t);
        }
    }
    return tx_len;
}


void udp_queue_feedback(uint32_t value_16_16)
{
    g_feedback_value = value_16_16;
    g_feedback_pending = true;
}