#include "udp.h"
#include "lwip/sockets.h"
#include "stdio.h"
#include "pin_mux.h"
#include "fsl_gpio.h"
#include "wifi_app.h"
#include "log.h"
#include "mode.h"
#include <math.h>

// Debug verbosity: 0 = errors/one-time only, 1 = warnings + packet loss, 2 = verbose
#define UDP_DEBUG_LVL 1

#define UDP_AUDIO_PORT 5000
#define SOCKET_RETRY_DELAY_MS 1000
#define IP_AP "192.168.1.1"
#define IP_STA "192.168.1.2"


// TODO: Revise all the buffer sizes in this file and #define them
// TODO: Consider making buffer sizes configurable via cli.
#define UDP_SPK_BUF_SIZE   (192*30)
#define UDP_MIC_BUF_SIZE   (192*30)

TaskHandle_t g_udp_rx_task_handle = NULL;
TaskHandle_t g_udp_tx_task_handle = NULL;

static volatile bool g_feedback_pending = false;
static volatile uint32_t g_feedback_value = 0;

// --- Tone Generator ---
#define TONE_FREQ_HZ    1000
#define TONE_AMPLITUDE  8192  // ~25% of full scale (32767)
#define TONE_PHASE_INC  ((uint32_t)(((double)TONE_FREQ_HZ / 48000.0) * 4294967296.0))

static int16_t  s_sine_table[256];
static bool     s_sine_table_init = false;
static uint32_t s_tone_phase = 0;
static volatile uint32_t g_tone_rx_feedback = 0;

static void udp_tone_init(void)
{
    if (!s_sine_table_init) {
        for (int i = 0; i < 256; i++)
            s_sine_table[i] = (int16_t)(TONE_AMPLITUDE * sinf(2.0f * 3.14159265f * i / 256.0f));
        s_sine_table_init = true;
    }
    s_tone_phase = 0;
}

static void udp_tone_generate(uint8_t *buf, uint16_t len)
{
    int16_t *samples = (int16_t *)buf;
    uint16_t n_samples = len / sizeof(int16_t);
    for (uint16_t i = 0; i < n_samples; i += 2) {
        int16_t s = s_sine_table[s_tone_phase >> 24];
        samples[i]   = s;  // Left
        samples[i+1] = s;  // Right
        s_tone_phase += TONE_PHASE_INC;
    }
}

uint16_t tx_udp_packet_counter = 0;
uint16_t rx_udp_packet_counter = 0;
static bool rx_spk_synced = false;

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


// Volatile because RX writes it, TX reads it.
// Initialize to -1 to indicate "Not Ready"
volatile int g_udp_sock = -1; 

// IP Addresses (Pre-calculated constants)
// Ideally these are defined in a header or passed in
#define IP_AP_INT  inet_addr(IP_AP)
#define IP_STA_INT inet_addr(IP_STA)


static void udp_process_rx(uint8_t *buffer, int len, app_mode_t mode);
static int udp_process_tx(uint8_t *buffer, app_mode_t mode);

const char* get_socket_error_string(int err) {
    switch (err) {
        case 9:  return "EBADF (Bad file descriptor / Socket closed)";
        case 11: return "EAGAIN / EWOULDBLOCK (Resource temporarily unavailable)";
        case 12: return "ENOMEM (Out of memory / LwIP pbuf pool empty)";
        case 103: return "ECONNABORTED (Software caused connection abort)";
        case 104: return "ECONNRESET (Connection reset by peer)";
        case 112: return "EHOSTDOWN (Host is down)";
        case 113: return "EHOSTUNREACH (No route to host)";
        case 118: return "ENOTCONN (Socket is not connected)";
        case 119: return "ETOOMANYREFS (Too many references / cannot splice)";
        case 128: return "ENOTCONN (Transport endpoint is not connected)";
        default:  return "Unknown Error";
    }
}

void udp_audio_ff_init(void)
{
    // Glue the Speaker FIFO to the Speaker Buffer
    // true = Overwritable (Circular Mode)
    tu_fifo_config(&udp_spk_ff, udp_spk_buf, UDP_SPK_BUF_SIZE, true);

    // Glue the Mic FIFO to the Mic Buffer
    tu_fifo_config(&udp_mic_ff, udp_mic_buf, UDP_MIC_BUF_SIZE, true);

    // Reset RX sequence state
    rx_udp_packet_counter = 0;
    rx_spk_synced = false;
}    

tu_fifo_t* udp_get_spk_fifo(void) {
    return &udp_spk_ff;
}    

tu_fifo_t* udp_get_mic_fifo(void) {
    return &udp_mic_ff;
}    


void udp_rx_task(void *pvParameters)
{
    struct sockaddr_in my_addr;
    static uint8_t rx_buffer[500]; // Static to save stack

    // Setup own address struct
    memset(&my_addr, 0, sizeof(my_addr));
    my_addr.sin_family = AF_INET;
    my_addr.sin_port = htons(UDP_AUDIO_PORT);
    my_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    for (;;)
    {
        // Blocking wait for WIFI_INIT_DONE
        xEventGroupWaitBits(g_wifi_events, WIFI_INIT_DONE, 
                            pdFALSE, pdTRUE, portMAX_DELAY);

        // Create Socket
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // Bind
        int ret = bind(sock, (struct sockaddr *)&my_addr, sizeof(my_addr));
        if (ret != 0) {
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // Configure QoS to Voice/Critical (0xE0)
        int tos = 0xE0;
        setsockopt(sock, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));

        // Configure Receive Timeout to 0 (Infinite Blocking)
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        
        // Publish Socket to TX Task
        // The TX task will see this variable change and start sending.
        g_udp_sock = sock;

        // Processing Loop
        // Runs as long as the socket is valid
        while (1) {
            int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0, NULL, NULL);
            if (len > 0) {
                // Process the packet
                udp_process_rx(rx_buffer, len, get_app_mode());
            } 
            else {
                PRINTF("Socket Error or Closed. Errno: %d\n", errno);
                break; // Break inner loop -> Close sock -> Wait for IP again
            }
        }

        // 8. Cleanup
        g_udp_sock = -1; // Stop TX task immediately
        PRINTF("Closing Socket...");
        close(sock);
    }
}


void udp_tx_task(void *pvParameters)
{
    struct sockaddr_in dest_addr;
    static uint8_t tx_buffer[500];

    // State tracking
    app_mode_t cached_mode = MODE_IDLE;

    // Tone mode timing state
    TickType_t tone_last_wake = 0;
    bool tone_timing_init = false;

    // Initialize Destination Struct
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(UDP_AUDIO_PORT);

    for (;;)
    {
        app_mode_t current_mode = get_app_mode();

        if (current_mode == MODE_UDP_DONGLE_TONE) {
            // Tone mode: pace ourselves with vTaskDelayUntil (no USB ISR to notify us)
            if (!tone_timing_init) {
                udp_tone_init();
                tone_last_wake = xTaskGetTickCount();
                tone_timing_init = true;
            }
            vTaskDelayUntil(&tone_last_wake, pdMS_TO_TICKS(2));
        } else {
            // Normal mode: wait for USB ISR or I2S notification
            tone_timing_init = false;
            ulTaskNotifyTake(pdFALSE, portMAX_DELAY);
            current_mode = get_app_mode();
        }

        // If socket isn't ready (RX task hasn't created it yet), skip.
        int sock = g_udp_sock; // Local copy to ensure atomicity
        if (sock < 0) {
            continue;
        }

        // Update Destination Address (if mode changed)
        if (current_mode != cached_mode) {
            if (current_mode == MODE_UDP_DONGLE_AUDIO ||
                current_mode == MODE_UDP_DONGLE_TONE) {
                dest_addr.sin_addr.s_addr = IP_STA_INT;
            } else {
                dest_addr.sin_addr.s_addr = IP_AP_INT;
            }
            cached_mode = current_mode;
        }

        // TX Processing
        int tx_len = udp_process_tx(tx_buffer, current_mode);

        if (tx_len > 0) {
            uint32_t t0 = DWT->CYCCNT;
            int sent = sendto(sock, tx_buffer, tx_len, 0,
                              (struct sockaddr *)&dest_addr, sizeof(dest_addr));
            uint32_t elapsed_us = (DWT->CYCCNT - t0) / (SystemCoreClock / 1000000u);
            if (elapsed_us > 1000) {
                PRINTF("TX STALL: sendto %ums seq=%u\n", elapsed_us / 1000, tx_udp_packet_counter);
            }
            if (sent < 0) {
                PRINTF("WARN: sendto() failed! tx_count=%u | errno=%d | error=\"%s\" \r\n",
                        tx_udp_packet_counter, errno, get_socket_error_string(errno));
            }
            // Always increment sequence so the headset sees a clean gap
            // rather than rejecting fresh packets as late/duplicate.
            tx_udp_packet_counter++;
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
            
            // Synchronize counter to dongle on first received packet to avoid
            // false loss detection when the dongle's counter is already ahead.
            if (!rx_spk_synced) {
                rx_udp_packet_counter = p_hdr->sequence;
                rx_spk_synced = true;
            }

            // Signed 16-bit arithmetic handles wraparound correctly:
            // late packets produce a negative diff, losses produce positive.
            int32_t diff = (int32_t)(int16_t)(p_hdr->sequence - rx_udp_packet_counter);

            // Drop late/out-of-order packets instantly
            if (diff < 0) {
#if UDP_DEBUG_LVL >= 1
                PRINTF("[UDP] WARN: Late packet dropped. Seq=%u Exp=%u\r\n", p_hdr->sequence, rx_udp_packet_counter);
#endif
                return;
            }

            // Drop packet if buffer is full
            if (tu_fifo_remaining(&udp_spk_ff) < payload_len) {
#if UDP_DEBUG_LVL >= 1
                PRINTF("[UDP] RX FULL remaining=%u\r\n", tu_fifo_remaining(&udp_spk_ff));
#endif
                rx_udp_packet_counter = p_hdr->sequence + 1;
                return;
            }

            // Handle packet loss: fill the gap with silence to maintain timing.
            // Insert silence as long as there is physical space for it + the real packet.
            if (diff > 0) {
#if UDP_DEBUG_LVL >= 1
                PRINTF("[UDP] WARN: %u lost. Exp=%u Got=%u\r\n",
                        diff,
                        rx_udp_packet_counter,
                        p_hdr->sequence);
#endif
                static const uint8_t silence[UDP_PACKET_SIZE] = {0};
                int32_t space = (int32_t)tu_fifo_remaining(&udp_spk_ff);
                int32_t insert_count = (space - (int32_t)payload_len) / (int32_t)payload_len;
                if (insert_count > diff) insert_count = diff;
                for (int32_t i = 0; i < insert_count; i++) {
                    tu_fifo_write_n(&udp_spk_ff, silence, payload_len);
                }
            }

            // --- REGULAR WRITE TO SPEAKER BUFFER ---
            tu_fifo_write_n(&udp_spk_ff, p_payload, payload_len);
            rx_udp_packet_counter = p_hdr->sequence + 1;
            // Wake the audio task
            if (g_audio_task_handle == NULL) {
                PRINTF("ERROR: Tried to wake audio task when handle is NULL pointer!\r\n");
            }
            else if (!q_full()) {
                //PRINTF("INFO: Waking audio_task from udp_rx_task!\n");
                xTaskNotifyGive(g_audio_task_handle); 
            }
        }
        else {
            PRINTF("ERROR: Received speaker audio when not in headset mode!\r\n");
            configASSERT(false);
        }
        break;

    case UDP_DATATYPE_MIC_AUDIO:
        // Only DONGLE should receive Mic Audio (from Headset)
        if (mode == MODE_UDP_DONGLE_AUDIO || mode == MODE_UDP_DONGLE_TONE) {
            tu_fifo_write_n(&udp_mic_ff, p_payload, payload_len);
        } else {
            PRINTF("ERROR: Received mic audio when not in dongle mode!\r\n");
            configASSERT(false);
        }
        break;

    case UDP_DATATYPE_FEEDBACK:
        if (mode == MODE_UDP_DONGLE_AUDIO) {
            uint32_t feedback_val;
            memcpy(&feedback_val, p_payload, sizeof(uint32_t));
            tud_audio_fb_set(feedback_val);
        } else if (mode == MODE_UDP_DONGLE_TONE) {
            uint32_t feedback_val;
            memcpy(&feedback_val, p_payload, sizeof(uint32_t));
            g_tone_rx_feedback = feedback_val;
        } else {
            // A Headset receiving feedback implies a logic error on the sender
            PRINTF("ERROR: Received Feedback packet when not in dongle/tone mode!\r\n");
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
            p_hdr->sequence = tx_udp_packet_counter;
            uint16_t read = tud_audio_read(p_payload, UDP_PACKET_SIZE);
            tx_len = sizeof(udp_header_t) + read;
        }
    }
    else if (mode == MODE_UDP_DONGLE_TONE)
    {
        // Generate one packet of 1kHz sine, ignoring USB entirely
        p_hdr->type = UDP_DATATYPE_SPEAKER_AUDIO;
        p_hdr->sequence = tx_udp_packet_counter;
        p_hdr->flags = 0;
        udp_tone_generate(p_payload, UDP_PACKET_SIZE);
        tx_len = sizeof(udp_header_t) + UDP_PACKET_SIZE;
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