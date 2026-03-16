#ifndef _LINK_TEST_H_
#define _LINK_TEST_H_

#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------
// Limits
// ---------------------------------------------------------------
#define RAW_LINKTEST_MAX_PAYLOAD 1400

// ---------------------------------------------------------------
// Payload headers
// ---------------------------------------------------------------
typedef struct __attribute__((packed)) {
    uint32_t tx_cyccnt;   // DWT timestamp at send
} lt_dn_header_t;        // 4 bytes + zero-padding

typedef struct __attribute__((packed)) {
    uint32_t echo_cyccnt; // copied from last received lt_dn_header_t.tx_cyccnt
    uint16_t echo_seq;    // sequence of the dongle packet being echoed
} lt_up_header_t;         // 6 bytes + zero-padding

// ---------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------
typedef struct {
    uint16_t dn_packet_size;   // dongle→headset payload bytes (default 384)
    uint16_t dn_interval_ms;   // dongle TX interval (default 2)
    uint16_t up_packet_size;   // headset→dongle payload bytes (default 32)
    uint16_t up_interval_ms;   // headset TX interval (default 25)
    uint32_t count;            // total packets to send per direction (0 = infinite)
    uint32_t spike_threshold_us; // IAT deviation threshold for spike counting (default 1000us)
    uint32_t duration_s;       // auto-stop after N seconds (0 = manual stop)
    bool     quiet;            // suppress periodic prints
} link_test_config_t;

// ---------------------------------------------------------------
// Stats (per-receiver)
// ---------------------------------------------------------------
typedef struct {
    uint32_t rx_count;
    uint32_t rx_lost;
    uint16_t rx_last_seq;
    bool     rx_synced;

    // Inter-arrival deviation (|actual_delta - expected_delta|)
    uint32_t iat_min_us;       // min deviation
    uint32_t iat_max_us;       // max deviation
    uint64_t iat_sum_us;       // sum for mean
    uint32_t iat_count;        // number of samples
    uint32_t last_arrival_cyc; // DWT->CYCCNT of last arrival
    uint32_t spike_count;      // number of IAT deviations > spike_threshold_us

    // IAT histogram: [0]<100us [1]100-500us [2]500us-1ms [3]1-5ms [4]5-10ms [5]>=10ms
    uint32_t iat_hist[6];

    // Throughput (bytes received in current 1s window)
    uint32_t tput_bytes;
    uint32_t tput_window_start; // tick count of window start
    uint32_t tput_last_bps;     // last computed bytes/sec

    // RTT (dongle only)
    uint32_t rtt_min_us;
    uint32_t rtt_max_us;
    uint64_t rtt_sum_us;
    uint32_t rtt_count;
} link_test_stats_t;

// ---------------------------------------------------------------
// API
// ---------------------------------------------------------------

// Get/set config (call before starting mode)
link_test_config_t *link_test_get_config(void);

// Reset stats and TX counters — called on mode entry
void link_test_reset(void);

// RX handlers — called from raw_process_rx()
void link_test_rx_dn(const uint8_t *data, int data_len, uint16_t seq);
void link_test_rx_up(const uint8_t *data, int data_len, uint16_t seq);

// TX builders — called from raw_process_tx()
// Returns payload length written to buf, or 0 if nothing to send
int link_test_build_dn(uint8_t *buf, uint16_t *seq_out);
int link_test_build_up(uint8_t *buf, uint16_t *seq_out);

// CLI: print current stats snapshot
void link_test_print_stats(void);

// CLI: print final summary and reset
void link_test_print_final(void);

// Periodic 1s summary — called from a timer or task
void link_test_periodic_print(void);

// Check if test is done (count reached)
bool link_test_tx_done(void);

// Check if timed test has expired (called from tx_task)
bool link_test_time_expired(void);

// Auto-stop: print final summary and switch to MODE_IDLE
void link_test_auto_stop(void);

#endif /* _LINK_TEST_H_ */
