#include "radio.h"
#include <hal/nrf_clock.h>
#include <soc.h>

/***************************************************************************
 * Shared state
 ***************************************************************************/

struct radio_packet pkt_tx;
struct radio_packet pkt_rx[2];
volatile uint8_t rx_buf_idx;
bool is_tx;

K_SEM_DEFINE(tx_done_sem, 1, 1);
K_SEM_DEFINE(rx_sem, 0, 1);
volatile int16_t rx_rssi;
volatile uint8_t rx_ready_idx;

/***************************************************************************
 * HFCLK startup
 ***************************************************************************/

static void hfclk_start(void)
{
    nrf_clock_event_clear(NRF_CLOCK, NRF_CLOCK_EVENT_HFCLKSTARTED);
    nrf_clock_task_trigger(NRF_CLOCK, NRF_CLOCK_TASK_HFCLKSTART);
    while (!nrf_clock_event_check(NRF_CLOCK, NRF_CLOCK_EVENT_HFCLKSTARTED)) {
        /* spin */
    }
}

/***************************************************************************
 * Radio ISR - dispatches based on runtime role
 ***************************************************************************/

static void radio_isr(const void *arg)
{
    (void)arg;

    if (is_tx) {
        if (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_DISABLED)) {
            nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
            k_sem_give(&tx_done_sem);
        }
    } else {
        if (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_CRCERROR)) {
            nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CRCERROR);
            nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_START);
            return;
        }

        if (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_PHYEND)) {
            nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_PHYEND);

            rx_rssi = nrf_radio_rssi_sample_get(NRF_RADIO);

            rx_ready_idx = rx_buf_idx;
            rx_buf_idx ^= 1;
            nrf_radio_packetptr_set(NRF_RADIO, (uint32_t *)&pkt_rx[rx_buf_idx]);

            nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_START);

            k_sem_give(&rx_sem);
        }
    }
}

/***************************************************************************
 * Radio initialization
 ***************************************************************************/

void radio_init(void)
{
    hfclk_start();

    nrf_radio_mode_set(NRF_RADIO, MODE);
    nrf_radio_frequency_set(NRF_RADIO, FREQ);
    nrf_radio_txpower_set(NRF_RADIO, TXPOWER);

    nrf_radio_packet_conf_t pkt_conf = {
        .lflen = 0,
        .s0len = 0,
        .s1len = 0,
        .s1incl = false,
        .plen = NRF_RADIO_PREAMBLE_LENGTH_8BIT,
        .crcinc = false,
        .maxlen = sizeof(struct radio_packet),
        .statlen = sizeof(struct radio_packet),
        .balen = 3,
        .big_endian = false,
        .whiteen = true,
    };
    nrf_radio_packet_configure(NRF_RADIO, &pkt_conf);

    nrf_radio_datawhiteiv_set(NRF_RADIO, (FREQ - 2400) & 0x3F);

    nrf_radio_crc_configure(NRF_RADIO,
        2,
        NRF_RADIO_CRC_ADDR_SKIP,
        0x11021UL);

    nrf_radio_base0_set(NRF_RADIO, SYNC_WORD);
    nrf_radio_txaddress_set(NRF_RADIO, 0);
    nrf_radio_rxaddresses_set(NRF_RADIO, 1 << 0);

    IRQ_CONNECT(RADIO_0_IRQn, 0, radio_isr, NULL, 0);
}
