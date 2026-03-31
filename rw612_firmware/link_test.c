#include "link_test.h"
#include "mode.h"
#include "log.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

// ARM DWT cycle counter
#include "fsl_device_registers.h"

// ---------------------------------------------------------------
// Config (defaults mimic real audio)
// ---------------------------------------------------------------
static link_test_config_t s_cfg = {
    .dn_packet_size     = 384,
    .dn_interval_ms     = 2,
    .up_packet_size     = 32,
    .up_interval_ms     = 25,
    .count              = 0,
    .spike_threshold_us = 1000,
    .duration_s         = 0,
    .quiet              = false,
};

link_test_config_t *link_test_get_config(void) { return &s_cfg; }

// ---------------------------------------------------------------
// Stats
// ---------------------------------------------------------------
static link_test_stats_t s_dn_stats; // dongle→headset (headset is receiver)
static link_test_stats_t s_up_stats; // headset→dongle (dongle is receiver)

// TX counters
static uint16_t s_tx_dn_seq;
static uint32_t s_tx_dn_count;
static uint16_t s_tx_up_seq;
static uint32_t s_tx_up_count;

// Headset: last received dongle timestamp+seq for echo
static volatile uint32_t s_echo_cyccnt;
static volatile uint16_t s_echo_seq;
static volatile bool     s_echo_valid;

// Periodic print tick
static uint32_t s_last_print_tick;

// Timed test state
static uint32_t s_start_tick;
static volatile bool s_test_done;

// ---------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------
static inline uint32_t cyc_to_us(uint32_t cycles)
{
    return cycles / (SystemCoreClock / 1000000u);
}

static void stats_update_iat(link_test_stats_t *st, uint32_t expected_interval_us)
{
    uint32_t now = DWT->CYCCNT;
    if (st->last_arrival_cyc != 0) {
        uint32_t delta_us = cyc_to_us(now - st->last_arrival_cyc);
        int32_t deviation = (int32_t)delta_us - (int32_t)expected_interval_us;
        uint32_t abs_dev = (deviation < 0) ? (uint32_t)(-deviation) : (uint32_t)deviation;

        if (abs_dev < st->iat_min_us) st->iat_min_us = abs_dev;
        if (abs_dev > st->iat_max_us) st->iat_max_us = abs_dev;
        st->iat_sum_us += abs_dev;
        st->iat_count++;

        if (abs_dev > s_cfg.spike_threshold_us)
            st->spike_count++;

        // Histogram bucketing on absolute deviation
        if (abs_dev < 100)           st->iat_hist[0]++;
        else if (abs_dev < 500)      st->iat_hist[1]++;
        else if (abs_dev < 1000)     st->iat_hist[2]++;
        else if (abs_dev < 5000)     st->iat_hist[3]++;
        else if (abs_dev < 10000)    st->iat_hist[4]++;
        else                         st->iat_hist[5]++;
    }
    st->last_arrival_cyc = now;
}

static void stats_update_throughput(link_test_stats_t *st, uint32_t bytes)
{
    uint32_t now = xTaskGetTickCount();
    st->tput_bytes += bytes;

    uint32_t elapsed = now - st->tput_window_start;
    if (elapsed >= pdMS_TO_TICKS(1000)) {
        // Compute bytes/sec
        st->tput_last_bps = (st->tput_bytes * 1000u) / (elapsed * portTICK_PERIOD_MS);
        st->tput_bytes = 0;
        st->tput_window_start = now;
    }
}

static void stats_update_seq(link_test_stats_t *st, uint16_t seq)
{
    if (!st->rx_synced) {
        st->rx_last_seq = seq;
        st->rx_synced = true;
        st->rx_count++;
        return;
    }

    uint16_t expected = st->rx_last_seq + 1;
    int32_t diff = (int32_t)(int16_t)(seq - expected);

    if (diff > 0) {
        st->rx_lost += (uint32_t)diff;
    }
    // Accept packet (even if late/duplicate, just count it)
    st->rx_count++;
    st->rx_last_seq = seq;
}

// ---------------------------------------------------------------
// Reset
// ---------------------------------------------------------------
void link_test_reset(void)
{
    memset(&s_dn_stats, 0, sizeof(s_dn_stats));
    memset(&s_up_stats, 0, sizeof(s_up_stats));
    s_tx_dn_seq   = 0;
    s_tx_dn_count = 0;
    s_tx_up_seq   = 0;
    s_tx_up_count = 0;
    s_echo_valid  = false;
    s_last_print_tick = xTaskGetTickCount();
    s_start_tick      = xTaskGetTickCount();
    s_test_done       = false;
    s_dn_stats.iat_min_us = UINT32_MAX;
    s_up_stats.iat_min_us = UINT32_MAX;
    s_dn_stats.rtt_min_us = UINT32_MAX;
    s_up_stats.rtt_min_us = UINT32_MAX;
    s_dn_stats.tput_window_start = xTaskGetTickCount();
    s_up_stats.tput_window_start = xTaskGetTickCount();
}

// ---------------------------------------------------------------
// RX: dongle→headset packet received on headset
// ---------------------------------------------------------------
void link_test_rx_dn(const uint8_t *data, int data_len, uint16_t seq)
{
    if (data_len < (int)sizeof(lt_dn_header_t)) return;

    const lt_dn_header_t *hdr = (const lt_dn_header_t *)data;

    // Save echo info for next UP packet
    s_echo_cyccnt = hdr->tx_cyccnt;
    s_echo_seq    = seq;
    s_echo_valid  = true;

    stats_update_seq(&s_dn_stats, seq);
    stats_update_iat(&s_dn_stats, (uint32_t)s_cfg.dn_interval_ms * 1000u);
    stats_update_throughput(&s_dn_stats, (uint32_t)data_len);
}

// ---------------------------------------------------------------
// RX: headset→dongle packet received on dongle
// ---------------------------------------------------------------
void link_test_rx_up(const uint8_t *data, int data_len, uint16_t seq)
{
    if (data_len < (int)sizeof(lt_up_header_t)) return;

    const lt_up_header_t *hdr = (const lt_up_header_t *)data;

    stats_update_seq(&s_up_stats, seq);
    stats_update_iat(&s_up_stats, (uint32_t)s_cfg.up_interval_ms * 1000u);
    stats_update_throughput(&s_up_stats, (uint32_t)data_len);

    // Compute RTT from echoed timestamp
    if (hdr->echo_cyccnt != 0) {
        uint32_t rtt_cyc = DWT->CYCCNT - hdr->echo_cyccnt;
        uint32_t rtt_us  = cyc_to_us(rtt_cyc);

        if (rtt_us < s_up_stats.rtt_min_us) s_up_stats.rtt_min_us = rtt_us;
        if (rtt_us > s_up_stats.rtt_max_us) s_up_stats.rtt_max_us = rtt_us;
        s_up_stats.rtt_sum_us += rtt_us;
        s_up_stats.rtt_count++;
    }
}

// ---------------------------------------------------------------
// TX builders
// ---------------------------------------------------------------
int link_test_build_dn(uint8_t *buf, uint16_t *seq_out)
{
    if (s_cfg.count > 0 && s_tx_dn_count >= s_cfg.count)
        return 0;

    uint16_t psize = s_cfg.dn_packet_size;
    if (psize > RAW_LINKTEST_MAX_PAYLOAD)
        psize = RAW_LINKTEST_MAX_PAYLOAD;
    if (psize < sizeof(lt_dn_header_t))
        psize = sizeof(lt_dn_header_t);

    memset(buf, 0, psize);
    lt_dn_header_t *hdr = (lt_dn_header_t *)buf;
    hdr->tx_cyccnt = DWT->CYCCNT;

    *seq_out = s_tx_dn_seq++;
    s_tx_dn_count++;
    return (int)psize;
}

int link_test_build_up(uint8_t *buf, uint16_t *seq_out)
{
    if (s_cfg.count > 0 && s_tx_up_count >= s_cfg.count)
        return 0;

    uint16_t psize = s_cfg.up_packet_size;
    if (psize > RAW_LINKTEST_MAX_PAYLOAD)
        psize = RAW_LINKTEST_MAX_PAYLOAD;
    if (psize < sizeof(lt_up_header_t))
        psize = sizeof(lt_up_header_t);

    memset(buf, 0, psize);
    lt_up_header_t *hdr = (lt_up_header_t *)buf;

    if (s_echo_valid) {
        hdr->echo_cyccnt = s_echo_cyccnt;
        hdr->echo_seq    = s_echo_seq;
    }

    *seq_out = s_tx_up_seq++;
    s_tx_up_count++;
    return (int)psize;
}

// ---------------------------------------------------------------
// Stats printing
// ---------------------------------------------------------------
static void print_stats_line(const char *dir, const link_test_stats_t *st, bool show_rtt)
{
    uint32_t total = st->rx_count + st->rx_lost;
    uint32_t loss_pct_x10 = 0;
    if (total > 0)
        loss_pct_x10 = (st->rx_lost * 1000u) / total;

    PRINTF("[LT] %s: rx=%lu lost=%lu (%lu.%lu%%) tput=%luB/s",
           dir,
           (unsigned long)st->rx_count,
           (unsigned long)st->rx_lost,
           (unsigned long)(loss_pct_x10 / 10),
           (unsigned long)(loss_pct_x10 % 10),
           (unsigned long)st->tput_last_bps);

    if (st->iat_count > 0) {
        uint32_t avg = (uint32_t)(st->iat_sum_us / st->iat_count);
        PRINTF(" iat_dev min=%lu avg=%lu max=%luus spikes(>%luus)=%lu",
               (unsigned long)st->iat_min_us,
               (unsigned long)avg,
               (unsigned long)st->iat_max_us,
               (unsigned long)s_cfg.spike_threshold_us,
               (unsigned long)st->spike_count);
    }

    if (show_rtt && st->rtt_count > 0) {
        uint32_t avg = (uint32_t)(st->rtt_sum_us / st->rtt_count);
        PRINTF(" rtt min=%lu avg=%lu max=%luus",
               (unsigned long)st->rtt_min_us,
               (unsigned long)avg,
               (unsigned long)st->rtt_max_us);
    }
    PRINTF("\r\n");
}

void link_test_print_stats(void)
{
    app_mode_t mode = get_app_mode();
    PRINTF("[LT] --- Link Test Stats ---\r\n");
    PRINTF("[LT] Config: dn=%uB/%ums up=%uB/%ums count=%lu\r\n",
           s_cfg.dn_packet_size, s_cfg.dn_interval_ms,
           s_cfg.up_packet_size, s_cfg.up_interval_ms,
           (unsigned long)s_cfg.count);

    const link_test_stats_t *hist_st = NULL;
    if (mode == MODE_RAW_DONGLE_LINKTEST) {
        PRINTF("[LT] TX dn: seq=%u count=%lu\r\n", s_tx_dn_seq, (unsigned long)s_tx_dn_count);
        print_stats_line("RX up", &s_up_stats, true);
        hist_st = &s_up_stats;
    } else if (mode == MODE_RAW_HEADSET_LINKTEST) {
        PRINTF("[LT] TX up: seq=%u count=%lu\r\n", s_tx_up_seq, (unsigned long)s_tx_up_count);
        print_stats_line("RX dn", &s_dn_stats, false);
        hist_st = &s_dn_stats;
    }
    if (hist_st && hist_st->iat_count > 0) {
        PRINTF("[LT] IAT histogram: <100us=%lu 100-500=%lu 500-1k=%lu 1k-5k=%lu 5k-10k=%lu >=10k=%lu\r\n",
               (unsigned long)hist_st->iat_hist[0],
               (unsigned long)hist_st->iat_hist[1],
               (unsigned long)hist_st->iat_hist[2],
               (unsigned long)hist_st->iat_hist[3],
               (unsigned long)hist_st->iat_hist[4],
               (unsigned long)hist_st->iat_hist[5]);
    }
    PRINTF("[LT] ------------------------\r\n");
}

void link_test_print_final(void)
{
    PRINTF("\r\n[LT] ====== FINAL SUMMARY ======\r\n");
    link_test_print_stats();
    PRINTF("[LT] ============================\r\n\r\n");
}

void link_test_periodic_print(void)
{
    uint32_t now = xTaskGetTickCount();

    // Check timed test expiration
    if (s_cfg.duration_s > 0 && !s_test_done) {
        uint32_t elapsed_ms = (now - s_start_tick) * portTICK_PERIOD_MS;
        if (elapsed_ms >= s_cfg.duration_s * 1000u) {
            s_test_done = true;
            return;
        }
    }

    if ((now - s_last_print_tick) < pdMS_TO_TICKS(1000))
        return;
    s_last_print_tick = now;

    if (s_cfg.quiet)
        return;

    app_mode_t mode = get_app_mode();
    if (mode == MODE_RAW_DONGLE_LINKTEST) {
        print_stats_line("RX up", &s_up_stats, true);
    } else if (mode == MODE_RAW_HEADSET_LINKTEST) {
        print_stats_line("RX dn", &s_dn_stats, false);
    }
}

bool link_test_tx_done(void)
{
    if (s_cfg.count == 0) return false;
    app_mode_t mode = get_app_mode();
    if (mode == MODE_RAW_DONGLE_LINKTEST)
        return s_tx_dn_count >= s_cfg.count;
    if (mode == MODE_RAW_HEADSET_LINKTEST)
        return s_tx_up_count >= s_cfg.count;
    return false;
}

bool link_test_time_expired(void)
{
    return s_test_done;
}

void link_test_auto_stop(void)
{
    link_test_print_final();
    set_current_app_mode(MODE_IDLE);
}
