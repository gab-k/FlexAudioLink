#include "udp_tasks.h"

#define UDP_AUDIO_PORT 5000
#define SOCKET_RETRY_DELAY_MS 1000
#define IP_AP "192.168.1.1"     // Dongle IP (Correct)
#define IP_STA "192.168.1.255"  // Broadcast Address for 192.168.1.x subnet


TaskHandle_t g_udp_task_handle = NULL;


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
            // Placeholder for received UDP packets...
            // process_incoming_udp_audio(buffer, len);
        }

        // TX STEP: Check for Outgoing Audio or Feedback Data
        // If there is data ready to send, send it now.
        int tx_len = 0; //get_tx_audio_data(buffer); commented out for now. 
        if (tx_len > 0) {
            sendto(sock, buffer, tx_len, 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        }
    }
}