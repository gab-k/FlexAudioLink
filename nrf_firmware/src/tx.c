#include "radio.h"
#include <zephyr/sys/printk.h>
#include <string.h>

static uint16_t tx_seq;

static void tx_timer_cb(struct k_timer *t)
{
    (void)t;

    if (k_sem_take(&tx_done_sem, K_NO_WAIT) != 0) {
        return;
    }

    nrf_radio_packetptr_set(NRF_RADIO, (uint32_t *)&pkt_tx);

    pkt_tx.seq = tx_seq++;
    memset(pkt_tx.data, 0xAB, sizeof(pkt_tx.data));

    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
    nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_TXEN);
}

static K_TIMER_DEFINE(tx_timer, tx_timer_cb, NULL);

void run_tx(void)
{
    printk("\n=== FlexLink Radio Test TX ===\n");
    printk("Frequency: %u MHz\n", FREQ);
    printk("Rate: 4 Mbps\n");
    printk("TX Power: +8 dBm\n");
    printk("Interval: %u ms\n", INTERVAL_MS);
    printk("Payload: %u bytes\n", PAYLOAD_LEN);
    printk("Sync word: 0x%08lX\n\n", SYNC_WORD);

    nrf_radio_shorts_enable(NRF_RADIO,
        NRF_RADIO_SHORT_READY_START_MASK |
        NRF_RADIO_SHORT_PHYEND_DISABLE_MASK);

    nrf_radio_int_enable(NRF_RADIO,
        NRF_RADIO_INT_DISABLED_MASK);

    irq_enable(RADIO_0_IRQn);

    pkt_tx.seq = 0;
    memset(pkt_tx.data, 0xAB, sizeof(pkt_tx.data));

    k_timer_start(&tx_timer, K_MSEC(INTERVAL_MS), K_MSEC(INTERVAL_MS));

    while (1) {
        k_sleep(K_FOREVER);
    }
}
