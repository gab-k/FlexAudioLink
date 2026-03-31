#include "raw_audio.h"
#include "wifi_app.h"
#include "mode.h"
#include "audio.h"
#include "link_test.h"
#include "log.h"
#include "wlan.h"

#include <math.h>
#include <string.h>
#include <stdbool.h>

#include "lwip/pbuf.h"
#include "lwip/prot/ethernet.h"
#include "port/net/wm_net.h"

// We piggyback on EtherType 0x0800 (IPv4) because the NXP mlan driver only passes
// whitelisted EtherTypes (IP, ARP, EAPOL) up to netif->input. We mark our frames
// with IPv4 protocol 253 (IANA experimental) so our netif hook can intercept them
// before lwIP's IP stack ever processes them.
#define RAW_IP_PROTO    253u   // IANA experimental protocol number
#define RAW_IP_HDR_LEN  20u    // Minimal IPv4 header (no options)

// Debug verbosity: 0 = errors/one-time only, 1 = warnings + TX counter, 2 = verbose
#define RAW_DEBUG_LVL 1

// FIFO buffer sizes — same as UDP equivalents
#define RAW_SPK_BUF_SIZE (192 * 30)
#define RAW_MIC_BUF_SIZE (192 * 30)

// ---------------------------------------------------------------
// Task handles
// ---------------------------------------------------------------
TaskHandle_t g_raw_rx_task_handle = NULL;
TaskHandle_t g_raw_tx_task_handle = NULL;

// ---------------------------------------------------------------
// Shared protocol header (layout identical to udp_header_t)
// ---------------------------------------------------------------
typedef struct {
    uint8_t  type;
    uint8_t  flags;
    uint16_t sequence;
} raw_header_t;

typedef enum {
    RAW_DATATYPE_SPEAKER_AUDIO = 0,
    RAW_DATATYPE_MIC_AUDIO     = 1,
    RAW_DATATYPE_FEEDBACK      = 2,
    RAW_DATATYPE_COMMAND       = 3,
    RAW_DATATYPE_TEST_DN       = 4,
    RAW_DATATYPE_TEST_UP       = 5,
} raw_datatype_t;

// ---------------------------------------------------------------
// FIFOs
// ---------------------------------------------------------------
TU_ATTR_ALIGNED(4) static uint8_t raw_spk_buf[RAW_SPK_BUF_SIZE];
TU_ATTR_ALIGNED(4) static uint8_t raw_mic_buf[RAW_MIC_BUF_SIZE];

static tu_fifo_t raw_spk_ff;
static tu_fifo_t raw_mic_ff;

// ---------------------------------------------------------------
// State
// ---------------------------------------------------------------
static volatile bool     g_feedback_pending = false;
static volatile uint32_t g_feedback_value   = 0;

static uint8_t  g_peer_mac[6]       = {0};
static volatile bool g_peer_mac_valid = false;
static uint8_t  g_own_mac[6]        = {0};

static netif_input_fn g_original_input = NULL;
static struct netif  *g_hooked_netif   = NULL;

static uint16_t tx_raw_packet_counter = 0;
static uint16_t rx_raw_packet_counter = 0;
static bool     rx_spk_synced         = false;

// ---------------------------------------------------------------
// Tone generator (identical to UDP version)
// ---------------------------------------------------------------
#define TONE_FREQ_HZ   1000
#define TONE_AMPLITUDE 8192
#define TONE_PHASE_INC ((uint32_t)(((double)TONE_FREQ_HZ / 48000.0) * 4294967296.0))

static int16_t  s_sine_table[256];
static bool     s_sine_table_init = false;
static uint32_t s_tone_phase      = 0;

static void raw_tone_init(void)
{
    if (!s_sine_table_init) {
        for (int i = 0; i < 256; i++)
            s_sine_table[i] = (int16_t)(TONE_AMPLITUDE * sinf(2.0f * 3.14159265f * i / 256.0f));
        s_sine_table_init = true;
    }
    s_tone_phase = 0;
}

static void raw_tone_generate(uint8_t *buf, uint16_t len)
{
    int16_t *s = (int16_t *)buf;
    uint16_t n = len / sizeof(int16_t);
    for (uint16_t i = 0; i < n; i += 2) {
        int16_t v = s_sine_table[s_tone_phase >> 24];
        s[i] = v; s[i + 1] = v;
        s_tone_phase += TONE_PHASE_INC;
    }
}

// ---------------------------------------------------------------
// Public API
// ---------------------------------------------------------------
void raw_audio_ff_init(void)
{
    tu_fifo_config(&raw_spk_ff, raw_spk_buf, RAW_SPK_BUF_SIZE, true);
    tu_fifo_config(&raw_mic_ff, raw_mic_buf, RAW_MIC_BUF_SIZE, true);
    rx_raw_packet_counter = 0;
    rx_spk_synced         = false;
}

tu_fifo_t *raw_get_spk_fifo(void) { return &raw_spk_ff; }
tu_fifo_t *raw_get_mic_fifo(void) { return &raw_mic_ff;  }

void raw_audio_set_peer_mac(const uint8_t *mac)
{
    taskENTER_CRITICAL();
    memcpy(g_peer_mac, mac, 6);
    g_peer_mac_valid = true;
    taskEXIT_CRITICAL();
    PRINTF("[RAW] Peer MAC: %02X:%02X:%02X:%02X:%02X:%02X\r\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void raw_audio_reset_peer_mac(void)
{
    taskENTER_CRITICAL();
    g_peer_mac_valid = false;
    taskEXIT_CRITICAL();
}

void raw_queue_feedback(uint32_t value_16_16)
{
    g_feedback_value   = value_16_16;
    g_feedback_pending = true;
}

// ---------------------------------------------------------------
// RX processing — called from the netif hook (wifi driver task)
// ---------------------------------------------------------------
static void raw_process_rx(const uint8_t *payload, int payload_len,
                            const uint8_t *src_mac, app_mode_t mode)
{
    if (payload_len < (int)sizeof(raw_header_t)) return;

    const raw_header_t *hdr  = (const raw_header_t *)payload;
    const uint8_t      *data = payload + sizeof(raw_header_t);
    int                 data_len = payload_len - sizeof(raw_header_t);

    switch (hdr->type) {
    case RAW_DATATYPE_SPEAKER_AUDIO:
        if (mode != MODE_RAW_HEADSET_AUDIO) break;

        // Learn peer MAC from first received frame (dongle's uAP MAC)
        if (!g_peer_mac_valid)
            raw_audio_set_peer_mac(src_mac);

        if (!rx_spk_synced) {
            rx_raw_packet_counter = hdr->sequence;
            rx_spk_synced = true;
        }

        {
            int32_t diff = (int32_t)(int16_t)(hdr->sequence - rx_raw_packet_counter);

            if (diff < 0) {
#if RAW_DEBUG_LVL >= 1
                PRINTF("[RAW] WARN: Late packet dropped. Seq=%u Exp=%u\r\n", hdr->sequence, rx_raw_packet_counter);
#endif
                return;
            }

            if (tu_fifo_remaining(&raw_spk_ff) < (uint16_t)data_len) {
#if RAW_DEBUG_LVL >= 1
                PRINTF("[RAW] RX FULL remaining=%u\r\n", tu_fifo_remaining(&raw_spk_ff));
#endif
                rx_raw_packet_counter = hdr->sequence + 1;
                return;
            }

            if (diff > 0) {
#if RAW_DEBUG_LVL >= 1
                PRINTF("[RAW] WARN: %u lost. Exp=%u Got=%u\r\n",
                       diff, rx_raw_packet_counter, hdr->sequence);
#endif
                static const uint8_t silence[RAW_AUDIO_PACKET_SIZE] = {0};
                int32_t space = (int32_t)tu_fifo_remaining(&raw_spk_ff);
                int32_t ins   = (space - data_len) / data_len;
                if (ins > diff) ins = diff;
                for (int32_t i = 0; i < ins; i++)
                    tu_fifo_write_n(&raw_spk_ff, silence, data_len);
            }

            tu_fifo_write_n(&raw_spk_ff, data, data_len);
            rx_raw_packet_counter = hdr->sequence + 1;

#if RAW_DEBUG_LVL >= 2
            {
                static uint32_t s_w = 0;
                if (++s_w <= 5 || (s_w % 500) == 0)
                    PRINTF("[RAW] write #%lu count=%u remaining=%u\r\n",
                           (unsigned long)s_w,
                           tu_fifo_count(&raw_spk_ff),
                           tu_fifo_remaining(&raw_spk_ff));
            }
#endif

            if (g_audio_task_handle && !q_full())
                xTaskNotifyGive(g_audio_task_handle);
        }
        break;

    case RAW_DATATYPE_FEEDBACK:
        // Dongle receives feedback from headset
        if (mode == MODE_RAW_DONGLE_AUDIO || mode == MODE_RAW_DONGLE_TONE) {
            if (!g_peer_mac_valid)
                raw_audio_set_peer_mac(src_mac);
            if (data_len >= (int)sizeof(uint32_t)) {
                uint32_t fb;
                memcpy(&fb, data, sizeof(fb));
                if (mode == MODE_RAW_DONGLE_AUDIO)
                    tud_audio_fb_set(fb);
            }
        }
        break;

    case RAW_DATATYPE_TEST_DN:
        if (mode == MODE_RAW_HEADSET_LINKTEST) {
            if (!g_peer_mac_valid)
                raw_audio_set_peer_mac(src_mac);
            link_test_rx_dn(data, data_len, hdr->sequence);
        }
        break;

    case RAW_DATATYPE_TEST_UP:
        if (mode == MODE_RAW_DONGLE_LINKTEST) {
            if (!g_peer_mac_valid)
                raw_audio_set_peer_mac(src_mac);
            link_test_rx_up(data, data_len, hdr->sequence);
        }
        break;

    default:
        break;
    }
}

// ---------------------------------------------------------------
// Netif RX hook — replaces netif->input
// ---------------------------------------------------------------
static err_t raw_audio_netif_input(struct pbuf *p, struct netif *inp)
{
    if (p->len >= (SIZEOF_ETH_HDR + RAW_IP_HDR_LEN)) {
        const struct eth_hdr *eth = (const struct eth_hdr *)p->payload;

        if (htons(eth->type) == ETHTYPE_IP) {
            const uint8_t *ip = (const uint8_t *)p->payload + SIZEOF_ETH_HDR;
            if (ip[9] == RAW_IP_PROTO) {
#if RAW_DEBUG_LVL >= 2
                static uint32_t s_rx = 0;
                if (++s_rx <= 5 || (s_rx % 500) == 0)
                    PRINTF("[RAW] RX #%lu len=%u\r\n", (unsigned long)s_rx, p->tot_len);
#endif
                uint8_t src_mac[6];
                memcpy(src_mac, eth->src.addr, ETH_HWADDR_LEN);
                const uint8_t *payload     = ip + RAW_IP_HDR_LEN;
                int            payload_len = p->tot_len - SIZEOF_ETH_HDR - RAW_IP_HDR_LEN;

                raw_process_rx(payload, payload_len, src_mac, get_app_mode());
                pbuf_free(p);
                return ERR_OK;
            }
        }
    }
    return g_original_input(p, inp);
}

void raw_audio_install_hook(struct netif *netif)
{
    if (netif == NULL || g_hooked_netif == netif) return;
    g_original_input = netif->input;
    netif->input     = raw_audio_netif_input;
    g_hooked_netif   = netif;
    PRINTF("[RAW] Hook installed on netif %c%c%d\r\n",
           netif->name[0], netif->name[1], netif->num);
}

// ---------------------------------------------------------------
// TX — static frame buffer layout:
//   [eth_hdr 14B][fake IPv4 hdr 20B][raw_header_t 4B][audio payload up to 384B]
// ---------------------------------------------------------------
#define RAW_FRAME_HDR_SIZE (SIZEOF_ETH_HDR + RAW_IP_HDR_LEN + sizeof(raw_header_t))
#define RAW_FRAME_MAX_SIZE (RAW_FRAME_HDR_SIZE + RAW_AUDIO_PACKET_SIZE)

// Linktest needs up to 1400B payload; audio needs 384B. Size to the larger.
#define RAW_FRAME_MAX_SIZE_LT (RAW_FRAME_HDR_SIZE + RAW_LINKTEST_MAX_PAYLOAD)
#define RAW_TX_BUF_SIZE (RAW_FRAME_MAX_SIZE_LT > RAW_FRAME_MAX_SIZE ? RAW_FRAME_MAX_SIZE_LT : RAW_FRAME_MAX_SIZE)

static uint8_t s_tx_frame[RAW_TX_BUF_SIZE];

static err_t raw_send_frame(struct netif *netif, uint16_t payload_len)
{
    uint16_t frame_len = (uint16_t)(RAW_FRAME_HDR_SIZE + payload_len);

    /* Update IPv4 total length (IP header + raw_header_t + audio payload) */
    uint16_t ip_total = (uint16_t)(RAW_IP_HDR_LEN + sizeof(raw_header_t) + payload_len);
    uint8_t *ip = s_tx_frame + SIZEOF_ETH_HDR;
    ip[2] = (uint8_t)(ip_total >> 8);
    ip[3] = (uint8_t)(ip_total & 0xFF);

    struct pbuf *p = pbuf_alloc_reference(s_tx_frame, frame_len, PBUF_REF);
    if (p == NULL) {
        PRINTF("[RAW] TX: pbuf_alloc_reference failed\r\n");
        return ERR_MEM;
    }
    err_t err = netif->linkoutput(netif, p);
    pbuf_free(p);
    if (err != ERR_OK)
        PRINTF("[RAW] TX err=%d\r\n", err);
    return err;
}

// Fill the ethernet + fake IPv4 header in s_tx_frame.
// Only needs to be called on mode change or after peer MAC is first set.
// The IPv4 total-length field is updated per-send in raw_send_frame().
static void raw_tx_build_eth_header(void)
{
    struct eth_hdr *eth = (struct eth_hdr *)s_tx_frame;
    memcpy(eth->dest.addr, g_peer_mac, ETH_HWADDR_LEN);
    memcpy(eth->src.addr,  g_own_mac,  ETH_HWADDR_LEN);
    eth->type = PP_HTONS(ETHTYPE_IP); /* 0x0800 — passes mlan EtherType filter */

    uint8_t *ip = s_tx_frame + SIZEOF_ETH_HDR;
    ip[0]  = 0x45;          /* Version=4, IHL=5 (20 bytes, no options) */
    ip[1]  = 0xE0;          /* DSCP/ECN — IP Precedence 7 → WMM AC_VO (Voice) */
    /* ip[2:3] total length — set per-send */
    ip[4]  = ip[5] = 0x00;  /* ID */
    ip[6]  = 0x40;          /* Flags: Don't Fragment */
    ip[7]  = 0x00;          /* Fragment offset */
    ip[8]  = 0x40;          /* TTL=64 */
    ip[9]  = RAW_IP_PROTO;  /* Protocol 253 — our audio marker */
    ip[10] = ip[11] = 0x00; /* Header checksum (0 = skip validation in hook) */
    ip[12] = ip[13] = ip[14] = ip[15] = 0x00; /* Src IP — don't care */
    ip[16] = ip[17] = ip[18] = ip[19] = 0x00; /* Dst IP — don't care */
}

// Fill s_tx_frame[RAW_FRAME_HDR_SIZE..] with the next packet to send.
// Returns payload byte count, or 0 if nothing to send.
static int raw_process_tx(app_mode_t mode)
{
    raw_header_t *hdr     = (raw_header_t *)(s_tx_frame + SIZEOF_ETH_HDR + RAW_IP_HDR_LEN);
    uint8_t      *payload = s_tx_frame + RAW_FRAME_HDR_SIZE;
    int           plen    = 0;

    if (mode == MODE_RAW_DONGLE_AUDIO) {
        if (tud_audio_available() >= RAW_AUDIO_PACKET_SIZE) {
            hdr->type     = RAW_DATATYPE_SPEAKER_AUDIO;
            hdr->flags    = 0;
            hdr->sequence = tx_raw_packet_counter;
            tud_audio_read(payload, RAW_AUDIO_PACKET_SIZE);
            plen = RAW_AUDIO_PACKET_SIZE;
        }
    } else if (mode == MODE_RAW_DONGLE_TONE) {
        hdr->type     = RAW_DATATYPE_SPEAKER_AUDIO;
        hdr->flags    = 0;
        hdr->sequence = tx_raw_packet_counter;
        raw_tone_generate(payload, RAW_AUDIO_PACKET_SIZE);
        plen = RAW_AUDIO_PACKET_SIZE;
    } else if (mode == MODE_RAW_HEADSET_AUDIO) {
        if (g_feedback_pending) {
            taskENTER_CRITICAL();
            uint32_t val       = g_feedback_value;
            g_feedback_pending = false;
            taskEXIT_CRITICAL();

            hdr->type     = RAW_DATATYPE_FEEDBACK;
            hdr->flags    = 0;
            hdr->sequence = tx_raw_packet_counter;
            memcpy(payload, &val, sizeof(val));
            plen = sizeof(val);
        }
    } else if (mode == MODE_RAW_DONGLE_LINKTEST) {
        uint16_t seq;
        plen = link_test_build_dn(payload, &seq);
        if (plen > 0) {
            hdr->type     = RAW_DATATYPE_TEST_DN;
            hdr->flags    = 0;
            hdr->sequence = seq;
        }
    } else if (mode == MODE_RAW_HEADSET_LINKTEST) {
        uint16_t seq;
        plen = link_test_build_up(payload, &seq);
        if (plen > 0) {
            hdr->type     = RAW_DATATYPE_TEST_UP;
            hdr->flags    = 0;
            hdr->sequence = seq;
        }
    }

    return plen;
}

// ---------------------------------------------------------------
// Tasks
// ---------------------------------------------------------------

// raw_rx_task: all RX is handled in the netif hook; this task just
// holds a handle so mode.c can suspend/resume it symmetrically.
void raw_rx_task(void *pvParameters)
{
    (void)pvParameters;
    for (;;)
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

void raw_tx_task(void *pvParameters)
{
    (void)pvParameters;

    TickType_t  periodic_last_wake = 0;
    bool        periodic_init     = false;
    app_mode_t  cached_mode       = MODE_IDLE;
    struct netif *tx_netif        = NULL;
    bool        eth_hdr_built     = false;

    for (;;) {
        // Wait for WiFi to be ready before resolving the netif
        xEventGroupWaitBits(g_wifi_events, WIFI_INIT_DONE,
                            pdFALSE, pdTRUE, portMAX_DELAY);

        app_mode_t mode = get_app_mode();

        if (mode == MODE_RAW_DONGLE_TONE) {
            if (!periodic_init) {
                raw_tone_init();
                periodic_last_wake = xTaskGetTickCount();
                periodic_init      = true;
            }
            vTaskDelayUntil(&periodic_last_wake, pdMS_TO_TICKS(2));
        } else if (mode == MODE_RAW_DONGLE_LINKTEST) {
            if (!periodic_init) {
                periodic_last_wake = xTaskGetTickCount();
                periodic_init      = true;
            }
            vTaskDelayUntil(&periodic_last_wake, pdMS_TO_TICKS(link_test_get_config()->dn_interval_ms));
            link_test_periodic_print();
            if (link_test_time_expired()) {
                link_test_auto_stop();
                periodic_init = false;
                continue;
            }
        } else if (mode == MODE_RAW_HEADSET_LINKTEST) {
            if (!periodic_init) {
                periodic_last_wake = xTaskGetTickCount();
                periodic_init      = true;
            }
            vTaskDelayUntil(&periodic_last_wake, pdMS_TO_TICKS(link_test_get_config()->up_interval_ms));
            link_test_periodic_print();
            if (link_test_time_expired()) {
                link_test_auto_stop();
                periodic_init = false;
                continue;
            }
        } else {
            periodic_init = false;
            ulTaskNotifyTake(pdFALSE, portMAX_DELAY);
            mode = get_app_mode();
        }

        // Re-resolve netif and own MAC on mode change
        if (mode != cached_mode) {
            cached_mode   = mode;
            tx_netif      = NULL;
            eth_hdr_built = false;

            if (mode == MODE_RAW_DONGLE_AUDIO || mode == MODE_RAW_DONGLE_TONE ||
                mode == MODE_RAW_DONGLE_LINKTEST) {
                wlan_get_mac_address_uap(g_own_mac);
                tx_netif = net_get_uap_interface();
            } else if (mode == MODE_RAW_HEADSET_AUDIO || mode == MODE_RAW_HEADSET_LINKTEST) {
                wlan_get_mac_address(g_own_mac);
                tx_netif = net_get_sta_interface();
            }
        }

        if (tx_netif == NULL || !g_peer_mac_valid) continue;

        // Build ethernet header once after peer MAC becomes available
        if (!eth_hdr_built) {
            raw_tx_build_eth_header();
            eth_hdr_built = true;
        }

        int plen = raw_process_tx(mode);
        if (plen > 0) {
            uint32_t t0 = DWT->CYCCNT;
            err_t err = raw_send_frame(tx_netif, (uint16_t)plen);
            uint32_t elapsed_us = (DWT->CYCCNT - t0) / (SystemCoreClock / 1000000u);
#if RAW_DEBUG_LVL >= 1
            if (elapsed_us > 1000)
                PRINTF("[RAW] TX STALL: linkoutput %lums seq=%u\r\n", (unsigned long)(elapsed_us / 1000), tx_raw_packet_counter);
            if (err != ERR_OK)
                PRINTF("[RAW] TX FAIL: err=%d seq=%u\r\n", err, tx_raw_packet_counter);
#endif
#if RAW_DEBUG_LVL >= 2
            if (tx_raw_packet_counter == 0 || (tx_raw_packet_counter % 500) == 0)
                PRINTF("[RAW] TX seq=%u\r\n", tx_raw_packet_counter);
#endif
#if RAW_DEBUG_LVL < 1
            (void)err; (void)elapsed_us;
#endif
            tx_raw_packet_counter++;
        }
    }
}
