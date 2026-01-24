#include "udp_tasks.h"


void udp_client_task(void *pvParameters)
{
    int sock;
    struct sockaddr_in target_addr;
    int counter = 0;
    char msg[512];

    PRINTF("[UDP Client] Starting...\r\n");

    // 1. Create Socket
    if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        PRINTF("[UDP Client] Error: Socket creation failed\r\n");
        vTaskDelete(NULL);
    }

    // 2. Configure Target (The SoftAP's IP)
    memset(&target_addr, 0, sizeof(target_addr));
    target_addr.sin_family = AF_INET;
    target_addr.sin_port = htons(5000);
    target_addr.sin_addr.s_addr = inet_addr("192.168.1.1"); 

    while (1) {
        // 3. Prepare Message
        sprintf(msg, "COUNTERRERERERERRRRRRRRRRRRRRRRRRRRRRRRRRRERRERERRRRRRRRRRRRRRRRRRERERERRERERERRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRCNTR: %d", counter++);

        
        // 4. Send
        GPIO_PortToggle(BOARD_INITPINS_LATENCY_GPIO, BOARD_INITPINS_LATENCY_PORT, BOARD_INITPINS_LATENCY_PIN_MASK);
        GPIO_PortToggle(BOARD_INITPINS_LATENCY_GPIO, BOARD_INITPINS_LATENCY_PORT, BOARD_INITPINS_LATENCY_PIN_MASK);
        sendto(sock, msg, strlen(msg), 0, (struct sockaddr *)&target_addr, sizeof(target_addr));
        //PRINTF("[TX] Sent to 192.168.1.1: %s\r\n", msg);
        
        vTaskDelay(pdMS_TO_TICKS(5)); // Send every 1000 ms
    }
}

void udp_server_task(void *pvParameters)
{
    int sock;
    struct sockaddr_in server_addr, client_addr;
    char rx_buffer[512];
    socklen_t addr_len = sizeof(client_addr);

    PRINTF("[UDP Server] Starting on Port 5000...\r\n");

    // 1. Create Socket
    if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        PRINTF("[UDP Server] Error: Socket creation failed\r\n");
        vTaskDelete(NULL);
    }

    // 2. Bind to Port 5000
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(5000);
    server_addr.sin_addr.s_addr = INADDR_ANY; // Listen on all interfaces

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        PRINTF("[UDP Server] Error: Bind failed\r\n");
        close(sock);
        vTaskDelete(NULL);
    }

    PRINTF("[UDP Server] Listening...\r\n");

    while (1) {
        // 3. Receive Data (Blocking call)
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0,
                           (struct sockaddr *)&client_addr, &addr_len);
        
        if (len > 0) {
            GPIO_PortToggle(BOARD_INITPINS_LATENCY_GPIO, BOARD_INITPINS_LATENCY_PORT, BOARD_INITPINS_LATENCY_PIN_MASK);
            GPIO_PortToggle(BOARD_INITPINS_LATENCY_GPIO, BOARD_INITPINS_LATENCY_PORT, BOARD_INITPINS_LATENCY_PIN_MASK);
            rx_buffer[len] = 0; // Null terminate string
            //PRINTF("RX: %s\r\n", rx_buffer);
            //PRINTF("[RX] From %s: %s\r\n", inet_ntoa(client_addr.sin_addr), rx_buffer);
        }
    }
}