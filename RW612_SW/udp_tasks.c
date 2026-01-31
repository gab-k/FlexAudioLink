#include "udp_tasks.h"

#define UDP_AUDIO_PORT 5000
#define SOCKET_RETRY_DELAY_MS 1000
#define IP_AP "192.168.1.1"     // Dongle IP (Correct)
#define IP_STA "192.168.1.255"  // Broadcast Address for 192.168.1.x subnet


TaskHandle_t g_udp_task_handle = NULL;

#define UDP_SPK_BUF_SIZE   2048
#define UDP_MIC_BUF_SIZE   2048

TU_ATTR_ALIGNED(4) static uint8_t udp_spk_buf[UDP_SPK_BUF_SIZE];
TU_ATTR_ALIGNED(4) static uint8_t udp_mic_buf[UDP_MIC_BUF_SIZE];

static tu_fifo_t udp_spk_ff;
static tu_fifo_t udp_mic_ff;

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

/// UDP Header structure, in case of audio stream data transmission, the data follows this header!
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
    
    // [Bytes 4-7] Generic Payload
    // For Feedback: 16.16 Fixed point sample rate adjustment value.
    // For Volume Cmd: The volume level.
    // For Audio: Length of data following this header.
    uint32_t payload;

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
    uint8_t buffer[1024]; // Temp buffer
    
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

            // Configure Target IP based on our Role
            app_mode_t current_mode = get_app_mode();
            if (current_mode == MODE_UDP_DONGLE_AUDIO) {
                // We are AP, send to Headsets (Broadcast or Specific)
                dest_addr.sin_addr.s_addr = inet_addr(IP_STA);
            } else if(current_mode == MODE_UDP_HEADSET_AUDIO) {
                // We are Headset, send to Dongle (Gateway)
                dest_addr.sin_addr.s_addr = inet_addr(IP_AP);
            }
        }

        // RX STEP: Check for incoming data
        int len = recvfrom(sock, buffer, sizeof(buffer), 0, NULL, NULL);
        if (len > 0) {

            udp_header_t *hdr = (udp_header_t*)buffer;
            uint8_t *audio_data = &buffer[sizeof(udp_header_t)];
            
            switch (hdr->type)
            {
            case UDP_DATATYPE_SPEAKER_AUDIO:
                tu_fifo_write_n(&udp_spk_ff, audio_data, len - sizeof(udp_header_t));
                break;
            case UDP_DATATYPE_MIC_AUDIO:
                tu_fifo_write_n(&udp_mic_ff, audio_data, len - sizeof(udp_header_t));
                break;
            case UDP_DATATYPE_FEEDBACK:
                tud_audio_fb_set(hdr->payload);
                break;
            case UDP_DATATYPE_COMMAND:
                // TODO: Implement command handling (Mute, Volume, Play/Pause)
                break;
            default:
                PRINTF("Unknown UDP Data Type: %d\r\n", hdr->type);
                break;
            }
        }

        // TX STEP: Check for Outgoing Audio or Feedback Data
        // If there is data ready to send, send it now.
        int tx_len = 0; //get_tx_audio_data(buffer); commented out for now. 
        if (tx_len > 0) {
            sendto(sock, buffer, tx_len, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        }
    }
}