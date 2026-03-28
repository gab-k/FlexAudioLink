#include "radio.h"
#include <zephyr/sys/printk.h>
#include <zephyr/logging/log.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(radio_test, LOG_LEVEL_INF);

void run_rx(void)
{
    printk("\n=== FlexLink Radio Test RX ===\n");
    printk("Frequency: %u MHz\n", FREQ);
    printk("Rate: 4 Mbps\n");
    printk("Payload: %u bytes\n", PAYLOAD_LEN);
    printk("Sync word: 0x%08lX\n\n", SYNC_WORD);

    rx_buf_idx = 0;
    nrf_radio_packetptr_set(NRF_RADIO, (uint32_t *)&pkt_rx[0]);

    nrf_radio_shorts_enable(NRF_RADIO,
        NRF_RADIO_SHORT_READY_START_MASK |
        NRF_RADIO_SHORT_ADDRESS_RSSISTART_MASK);

    nrf_radio_int_enable(NRF_RADIO,
        NRF_RADIO_INT_CRCERROR_MASK |
        NRF_RADIO_INT_PHYEND_MASK);

    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_CRCERROR);
    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_PHYEND);
    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_READY);

    irq_enable(RADIO_0_IRQn);

    nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RXEN);

    uint16_t last_seq = 0;
    uint32_t total = 0;
    uint32_t lost = 0;
    int64_t rssi_sum = 0;
    int64_t last_time = 0;
    int64_t last_print = 0;
    uint32_t expected_interval_us = INTERVAL_MS * 1000;
    uint16_t consec_drop = 0;
    uint16_t max_burst_drop = 0;
    /* IAT deviation stats (in us) */
    int64_t iat_min = INT64_MAX;
    int64_t iat_max = 0;
    int64_t iat_sum = 0;
    uint32_t iat_count = 0;
    uint32_t iat_hist[6] = { 0 };

    while (1) {
        if (k_sem_take(&rx_sem, K_MSEC(100)) == 0) {
            uint8_t idx = rx_ready_idx;
            uint16_t seq = pkt_rx[idx].seq;
            int16_t rssi = rx_rssi;

            uint16_t expected = last_seq + 1;
            if (seq != expected && last_seq != 0) {
                uint16_t gap = (uint16_t)(seq - expected);
                /* Ignore seq wraparound */
                if (gap < 1000) {
                    lost += gap;
                    consec_drop += gap;
                    if (consec_drop > max_burst_drop) {
                        max_burst_drop = consec_drop;
                    }
                }
            } else {
                consec_drop = 0;
            }
            last_seq = seq;
            total++;
            rssi_sum += rssi;

            int64_t now = k_uptime_ticks();
            if (last_time != 0) {
                int64_t delta_us = k_ticks_to_us_near64(now - last_time);
                int64_t deviation = llabs(delta_us - expected_interval_us);

                if (deviation < iat_min) {
                    iat_min = deviation;
                }
                if (deviation > iat_max) {
                    iat_max = deviation;
                }
                iat_sum += deviation;
                iat_count++;

                if (deviation < 100) {
                    iat_hist[0]++;
                } else if (deviation < 500) {
                    iat_hist[1]++;
                } else if (deviation < 1000) {
                    iat_hist[2]++;
                } else if (deviation < 5000) {
                    iat_hist[3]++;
                } else if (deviation < 10000) {
                    iat_hist[4]++;
                } else {
                    iat_hist[5]++;
                }
            }
            last_time = now;
        }

        /* Print cumulative stats every 5 seconds */
        int64_t now_ms = k_uptime_get();
        if (now_ms - last_print >= 5000) {
            if (total > 0) {
                uint32_t total_pkts = total + lost;
                uint32_t loss_permyriad = (lost * 10000) / total_pkts;
                uint32_t loss_int = loss_permyriad / 100;
                uint32_t loss_frac = loss_permyriad % 100;
                int16_t avg_rssi = (int16_t)(rssi_sum / total);
                int64_t iat_avg = iat_count ? (iat_sum / iat_count) : 0;

                LOG_INF("--- RX Stats (cumulative) ---");
                LOG_INF("  pkts: %u  lost: %u (%u.%02u%%)  burst: %u  RSSI: %d dBm",
                        total, lost, loss_int, loss_frac, max_burst_drop, -avg_rssi);
                LOG_INF("  IAT dev  min: %lld us  avg: %lld us  max: %lld us",
                        iat_min == INT64_MAX ? 0 : iat_min, iat_avg, iat_max);
                LOG_INF("  IAT hist <100us: %u  <500us: %u  <1ms: %u  <5ms: %u  <10ms: %u  >10ms: %u",
                        iat_hist[0], iat_hist[1], iat_hist[2],
                        iat_hist[3], iat_hist[4], iat_hist[5]);
            }
            last_print = now_ms;
        }
    }
}
