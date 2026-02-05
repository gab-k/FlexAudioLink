#include "udp.h"

#define UDP_AUDIO_PORT 5000
#define SOCKET_RETRY_DELAY_MS 1000
#define IP_AP "192.168.1.1"     // Dongle IP (Correct)
#define IP_STA "192.168.1.255"  // Broadcast Address for 192.168.1.x subnet


TaskHandle_t g_udp_task_handle = NULL;

// TODO: Revise all the buffer sizes in this file and #define them
// TODO: Consider making buffer sizes configurable via cli.

#define UDP_SPK_BUF_SIZE   8192
#define UDP_MIC_BUF_SIZE   8192

#define UDP_SPK_AUDIO_PAYLOAD_SIZE (UDP_SPK_BUF_SIZE/4)
#define UDP_MIC_AUDIO_PAYLOAD_SIZE (UDP_MIC_BUF_SIZE/4)

TU_ATTR_ALIGNED(4) static uint8_t udp_spk_buf[UDP_SPK_BUF_SIZE];
TU_ATTR_ALIGNED(4) static uint8_t udp_mic_buf[UDP_MIC_BUF_SIZE];

static tu_fifo_t udp_spk_ff;
static tu_fifo_t udp_mic_ff;

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
            WIFI_EVENT_IP_ACQUIRED, // The Bit to wait for
            pdFALSE,                 // Don't clear the bit on exit (keep it set for others)
            pdTRUE,                  // Wait for the bit to be set
            portMAX_DELAY            // Wait forever
        );

        // Configure socket if not already done
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

            // Set Timeout (Crucial for bi-directional loop)
            // Wait 2ms for RX. If nothing, check TX.
            struct timeval tv = {0, 2000}; 
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
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

        // RX STEP: Block with timeout (2ms)
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0, NULL, NULL);
        if (len > 0) {
            udp_process_rx(rx_buffer, len, current_mode);
        }

        // TX STEP: Check if there is anything to send
        int tx_len = udp_process_tx(tx_buffer, current_mode);
        // Perform the Send
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
        PRINTF("Received packet too small: %d bytes\r\n", len);
        configASSERT(false);
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
        } else {
            PRINTF("[Error] Received speaker audio when not in headset mode!\r\n");
            configASSERT(false);
        }
        break;

    case UDP_DATATYPE_MIC_AUDIO:
        // Only DONGLE should receive Mic Audio (from Headset)
        if (mode == MODE_UDP_DONGLE_AUDIO) {
            tu_fifo_write_n(&udp_mic_ff, p_payload, payload_len);
        } else {
            PRINTF("[Error] Received mic audio when not in dongle mode!\r\n");
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
            PRINTF("[Error] Received Feedback packet when not in dongle mode!\r\n");
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
        if (available >= 1024) 
        {
            p_hdr->type = UDP_DATATYPE_SPEAKER_AUDIO;
            uint16_t read = tud_audio_read(p_payload, 1024);
            tx_len = sizeof(udp_header_t) + read;
        }
    }
    else if (mode == MODE_UDP_HEADSET_AUDIO)
    {
        // TODO: Implement Uplink Mic Audio and Feedback sending.
    }
    return tx_len;
}