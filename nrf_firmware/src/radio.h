#ifndef RADIO_H
#define RADIO_H

#include <zephyr/kernel.h>
#include <hal/nrf_radio.h>

/***************************************************************************
 * Configuration
 ***************************************************************************/

#define SYNC_WORD           0xD391A5D3UL

#define FREQ                2480        /* Absolute frequency in MHz */
#define TXPOWER             NRF_RADIO_TXPOWER_POS8DBM
#define MODE                NRF_RADIO_MODE_NRF_4MBIT_BT_0_6

#define INTERVAL_MS         1           /* 1 ms between packets */

#define PAYLOAD_LEN         250
#define PACKET_LEN          (2 + PAYLOAD_LEN)

#define TX_DEVICE_ID        0xb007ec2fe3f4e0c0ULL

/***************************************************************************
 * Packet structure
 ***************************************************************************/

struct radio_packet {
    uint16_t seq;
    uint8_t  data[PAYLOAD_LEN];
} __packed __aligned(4);

/***************************************************************************
 * Shared state (defined in radio.c)
 ***************************************************************************/

extern struct radio_packet pkt_tx;
extern struct radio_packet pkt_rx[2];
extern volatile uint8_t rx_buf_idx;
extern bool is_tx;

extern struct k_sem tx_done_sem;
extern struct k_sem rx_sem;
extern volatile int16_t rx_rssi;
extern volatile uint8_t rx_ready_idx;

/***************************************************************************
 * Functions
 ***************************************************************************/

void radio_init(void);
void run_tx(void);
void run_rx(void);

#endif /* RADIO_H */
