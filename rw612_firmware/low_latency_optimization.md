# Low-Latency Wireless Audio Optimization

This document tracks all low-latency optimizations implemented in FlexAudioLink,
and outlines future directions worth exploring.

---

## Current Latency Budget (Raw L2 Mode)

| Stage | Latency | Notes |
|-------|---------|-------|
| USB HS microframe arrival | ~0.125 ms | USB High-Speed polls every 125 us |
| Dongle processing + WiFi TX | ~0.1 ms | Build frame, submit to driver |
| WiFi air time (TX + ACK) | ~0.5-2 ms | CSMA/CA + transmission + acknowledgment |
| Headset RX processing | ~0.05 ms | Netif hook, FIFO write |
| **Buffer residence** | **~12.5 ms** | **50% of 25 ms buffer (dominant)** |
| I2S DMA pipeline | ~0.5-1 ms | Up to 4 DMA blocks in flight |
| **Total** | **~14-16 ms** | **Steady-state estimate** |

---

## Implemented Optimizations

### WiFi Driver Configuration (`wifi_app.c`)

#### Power Save Disabled
WiFi Power Save (PS) allows the radio to sleep between beacons to save energy.
The AP (Access Point) buffers packets for sleeping stations and delivers them
after a beacon. This adds tens of milliseconds of delay.

- IEEE Power Save off: `wlan_ieeeps_off()`
- Deep Sleep PS off: `wlan_deepsleepps_off()`
- Listen Interval = 1: Wake on every beacon (normally stations skip beacons to sleep longer)
- Null packet interval = 1s: Actively signals the AP that the STA (Station) is awake,
  preventing the AP from buffering packets
- WMM-UAPSD (Unscheduled Automatic Power Save Delivery) disabled:
  UAPSD lets stations sleep and wake only when specific traffic arrives.
  Good for phones, terrible for real-time audio.
- Runtime detection: `audio_task` checks `wlan_is_power_save_enabled()` every ~2 ms
  and force-disables PS if the firmware re-enables it autonomously

#### A-MPDU Disabled (Aggregate MAC Protocol Data Unit)
A-MPDU batches multiple WiFi frames into one large transmission.
Great for throughput (fewer ACKs, less overhead), terrible for latency
because the driver waits to collect frames before sending.

- TX and RX aggregation disabled on both AP and STA interfaces
- Each 384-byte audio frame is transmitted individually, immediately

#### RTS/CTS Disabled (Request To Send / Clear To Send)
RTS/CTS is a handshake mechanism that reserves the channel before data transmission.
It prevents collisions from hidden nodes (devices that can't hear each other but both
talk to the same AP). The handshake adds ~0.5 ms overhead per frame.

- RTS threshold set to 2347 (max frame size), effectively disabling it for all packets
- Safe because there's only one client on the dedicated AP

#### Background Activity Suppressed
Any background WiFi activity (scanning, roaming, regulatory lookups) temporarily
takes the radio away from data transfer.

- 802.11d disabled: Regulatory domain scanning off (the radio doesn't need to discover
  which country it's in for a dedicated link)
- 802.11k disabled: Neighbor report and radio measurement off (used for roaming decisions)
- Background scans disabled: No periodic channel scanning while streaming
- Roaming disabled: STA stays on its channel, never tries to find a "better" AP
- EDMAC (Energy Detect Multi-Address Correlation) disabled: A radar avoidance feature
  on some channels that can cause TX pauses

#### Task Priorities
- WiFi driver task: `configMAX_PRIORITIES - 3` (near highest)
- WiFi driver TX task: `configMAX_PRIORITIES - 3`
- WiFi scan task: `configMAX_PRIORITIES - 7` (lower, non-critical)

### Raw L2 Transport (`raw_audio.c`)

#### Bypassing the IP Stack
The standard UDP path goes: Application -> Socket API -> lwIP UDP -> lwIP IP ->
lwIP Ethernet -> WiFi driver. Each layer adds processing and potential queuing delay.

The raw L2 path bypasses all of this by:
1. Building Ethernet frames directly in a static buffer (pre-built header, zero-copy)
2. Calling `netif->linkoutput()` to hand the frame straight to the WiFi driver
3. On RX: installing a netif input hook that intercepts frames before lwIP processes them

The frames use EtherType 0x0800 (IPv4) to pass the WiFi driver's whitelist filter,
with IPv4 protocol number 253 (IANA experimental) as a marker. The netif hook checks
this protocol field and diverts matching frames before lwIP's IP layer ever sees them.

#### WMM Voice Priority (AC_VO)
WMM (WiFi Multimedia) defines four Access Categories for traffic prioritization:

| Access Category | CWmin | CWmax | AIFSN | Typical Use |
|-----------------|-------|-------|-------|-------------|
| AC_BK (Background) | 15 | 1023 | 7 | Downloads, backups |
| AC_BE (Best Effort) | 15 | 1023 | 3 | Web browsing, default |
| AC_VI (Video) | 7 | 15 | 2 | Video streaming |
| **AC_VO (Voice)** | **3** | **7** | **2** | **VoIP, real-time audio** |

CWmin/CWmax = Contention Window minimum/maximum. Before transmitting, a WiFi device
picks a random backoff slot in [0, CW]. With CWmin=3, the maximum initial wait is
3 slot times (~27 us) vs 15 slots (~135 us) for Best Effort. This means Voice
traffic gets on the air ~5x faster on average.

AIFSN (Arbitration Inter-Frame Spacing Number) = minimum idle time before contention.
Lower AIFSN = can start contending sooner after the channel becomes idle.

The raw L2 frames set IP header ToS = 0xE0 (IP Precedence 7), which the NXP WiFi
driver maps to AC_VO when selecting the WMM transmit queue.

### Audio Pipeline (`audio.c`)

#### DMA Block Sizing
The I2S DMA transfers audio from the FIFO to the codec. Block size is a tradeoff:
- Too large (e.g., 8 ms): adds buffering latency, wastes time waiting to fill
- Too small (e.g., 0.1 ms): excessive ISR overhead from frequent DMA completions

Current balance:
- MIN_DMA_BLOCK_SIZE = 192 bytes (~1 ms) - used only when DMA is running dry
- MAX_DMA_BLOCK_SIZE = 1536 bytes (~8 ms) - upper limit per transfer
- Up to 4 transfers in flight (SIZE_Q_DEPTH = I2S_NUM_BUFFERS)

#### Buffer State Machine
Two states to prevent playing silence or stuttering:
- **BUFFERING**: Accumulate audio until buffer is 50% full, then transition to PLAYING
- **PLAYING**: Feed DMA continuously. If buffer + in-flight both reach zero, transition
  back to BUFFERING

#### Feedback Loop
USB Audio Class 2 (UAC2) uses an adaptive clock model: the device tells the host how
fast it's consuming audio via a feedback endpoint. The host adjusts its sending rate
to match.

- Feedback task runs every 25 ms
- Computes buffer fill error: `target (50%) - filtered_level`
- Proportional controller: KP = 0.05, max adjustment +/-10 Hz around 48000 Hz
- Filtered level uses EWMA (Exponentially Weighted Moving Average) with alpha = 0.05
  to smooth out packet-level jitter
- Result sent as 16.16 fixed-point samples-per-microframe value
- In wireless modes, feedback is forwarded to the dongle via raw L2 or UDP, where
  the dongle sets it on the USB feedback endpoint

#### Packet Loss Handling
Both raw L2 and UDP modes use identical logic:
- Sequence number tracking with signed 16-bit wraparound arithmetic
- Late packets (sequence < expected): dropped, logged at debug level >= 1
- Missing packets (gap in sequence): silence inserted to maintain timing
- Buffer full: packet discarded, logged at debug level >= 1

### Network Stack (`lwipopts.h`)

#### Protocol Minimization
Every disabled protocol = less code running, fewer interrupts, less memory pressure:
- TCP: disabled entirely (UDP-only audio transport)
- IPv6: disabled (IPv4 only for DHCP/DNS setup; raw L2 doesn't even use IP)
- IGMP: disabled (no multicast needed for point-to-point audio)
- Raw sockets: disabled
- Statistics collection: disabled (avoids counter overhead in hot paths)

#### High-Performance Tuning
- Large PBUF pool (100 buffers): absorbs packet bursts without allocation failures
- Large mailbox sizes (64 for tcpip, 32 for UDP recv): prevents queue overflow under load
- Core locking enabled: mutex-based rather than message-passing for lower overhead

### Packet Format

Both raw L2 and UDP use the same 384-byte audio payload = 2 ms of audio:
- 48000 Hz sample rate * 2 channels * 2 bytes/sample = 192,000 bytes/sec
- 192 bytes = 1 ms of audio
- 384 bytes = 2 ms per packet, sent at 500 packets/sec
- 4-byte header: type (1B) + flags (1B) + sequence (2B)

---

## Known Issues

### WiFi Delivery Jitter (~5-20 ms stalls)
Even with all optimizations above, the headset occasionally experiences brief periods
where no packets are delivered by the WiFi driver, followed by a burst of queued packets.

**Evidence from testing:**
- First underflow typically occurs 20-30 minutes into playback
- Dongle TX shows zero errors (all packets sent successfully)
- No sequence gaps detected (packets aren't lost, they're delayed)
- Excellent signal: RSSI -32 dBm, SNR 60 dB
- Power Save confirmed NOT re-enabling during playback

**Cascade effect:** After an underflow, the burst of delayed packets fills the buffer
to 100%. Since arrival rate = consumption rate in steady state, the buffer stays at 100%
with zero headroom. Any subsequent WiFi hiccup immediately causes another underflow.
Events accelerate: 27 min -> 5 min -> 19 s -> 6 s -> 4 min intervals observed.

**Likely cause:** NXP mlan WiFi firmware internal queuing/scheduling, and/or CSMA/CA
contention from neighboring 5 GHz networks on the same channel. The 25 ms buffer (12.5 ms
headroom above 50% threshold) is too small to absorb these stalls without underflow.

---

## Future Exploration

### 1. EDCA Parameter Hacking (High Priority)

**What:** Override the WMM contention window parameters to near-zero values.

**Why it matters:** Even with AC_VO, the standard parameters (CWmin=3) still allow
neighboring networks to compete. By setting CWmin=0, CWmax=0, AIFSN=1 on both the
AP and STA, frames would transmit with minimal backoff — effectively winning every
contention against standard-configured neighbors.

**How it works:** EDCA (Enhanced Distributed Channel Access) parameters control how long
a device waits before transmitting. The wait time = AIFS + random(0, CW) slot times.
With CWmin=0 and AIFSN=1, the wait is just 1 inter-frame slot (~9 us at 5 GHz) with
no random component. Standard AC_BE devices wait at least 3 slots + random(0,15) slots.

**Feasibility:** The NXP SDK exposes `MLAN_OID_WMM_CFG_QUEUE_CONFIG` IOCTLs and
`wmm_ac_parameters_t` structures with `aci_aifsn` and `ecw` fields. Since both AP and
STA are controlled, the parameters can be set on both sides.

**Risk:** Violates 802.11 spec. In practice, the only consequence is being slightly
"unfair" to neighboring networks.

**Expected impact:** Significant reduction in jitter from external contention. Won't fix
mlan-internal delays but removes one major variable.


### 2. Packet Loss Concealment / PLC (High Priority)

**What:** Instead of inserting silence when packets are missing or late, generate
a plausible audio replacement.

**Why it matters:** The current approach inserts silence for missing packets. Even a
single 2 ms silence gap is audible as a click/pop. Good PLC makes occasional losses
nearly inaudible, which means the system can tolerate WiFi jitter without increasing
the buffer.

**Approaches (simplest to most complex):**
1. **Last-packet repeat:** Copy the previous 384-byte audio block. Simple, zero CPU cost,
   works well for single isolated losses. Sounds like a brief "freeze" rather than a click.
2. **Linear interpolation/crossfade:** Blend between last good packet and next good packet.
   Slightly better quality, requires one packet of look-ahead (adds 2 ms latency).
3. **Opus codec with inband FEC:** See below.

### 3. Opus Codec Integration (Medium Priority, High Effort)

**What:** Encode audio with the Opus codec instead of sending raw PCM.

**Why it matters (multiple benefits):**
- **Built-in PLC:** When the decoder receives no packet, it generates concealment audio
  based on the codec's internal state. Quality is significantly better than silence or
  sample repetition.
- **Inband FEC (Forward Error Correction):** Each Opus packet can carry a low-bitrate
  copy of the *previous* frame. If a packet is lost, the next packet contains enough
  information to reconstruct it. This recovers from single-packet losses with zero
  additional latency and minimal bandwidth cost.
- **Compression:** 384 bytes of raw PCM -> ~40-80 bytes with Opus at high quality.
  Smaller frames = faster WiFi transmission, less channel occupancy, less contention.
- **Frame sizes:** Opus supports 2.5 ms, 5 ms, 10 ms, 20 ms frames. A 2.5 ms or 5 ms
  frame adds only that much codec latency.

**Codec latency:** The Opus algorithmic delay at 2.5 ms frame size is 2.5 ms encode +
2.5 ms decode = 5 ms added to the pipeline.

**Feasibility concerns:**
- CPU: Opus encoding on Cortex-M33 @ 260 MHz may be tight. Decoding is cheaper.
  Would need profiling. The dongle encodes (has USB + WiFi but no I2S processing),
  the headset decodes (has WiFi + I2S DMA but no USB processing).
- Memory: Opus state requires ~10-30 KB depending on configuration.
- Integration effort: Significant — new dependency, encode/decode tasks, frame
  packetization changes.

### 4. Adaptive Buffer Sizing (Medium Priority)

**What:** Instead of a fixed 50% start threshold, dynamically adjust the buffer target
based on observed jitter.

**Why it matters:** A fixed 12.5 ms buffer is a compromise — too much latency for quiet
WiFi conditions, too little headroom for jittery conditions. An adaptive approach could:
- Start with a small buffer (e.g., 5 ms) for minimal latency
- Grow the target when jitter is detected (consecutive near-underflows)
- Shrink back toward minimum when the link is stable

**Related: faster feedback filter.** The current EWMA alpha (0.05) responds very slowly
to sudden buffer level changes. Increasing to 0.15-0.2 would let the feedback loop
track reality more closely, reducing the drift that leads to underflows.

### 5. Smaller Packets at Higher Rate (Low Priority)

**What:** Send 192-byte packets every 1 ms instead of 384 bytes every 2 ms.

**Why it matters:**
- Each lost packet = only 1 ms of audio (easier to conceal)
- Finer granularity for buffer management
- Trade: doubles WiFi overhead (2x more frames, 2x more ACKs, 2x more contention slots)

Probably not worth it unless combined with Opus (where the overhead per packet is the
WiFi/MAC framing, not the audio payload size).

### 6. Alternative Radio Technologies (Long Term)

If 802.11 proves fundamentally insufficient for < 10 ms:

**Nordic nRF5340 + Proprietary 2.4 GHz:**
The approach used by gaming headsets (SteelSeries, Corsair, etc.).
A dedicated 2.4 GHz transceiver with a custom TDMA (Time Division Multiple Access)
protocol: fixed timeslots, no contention, deterministic delivery every N ms.
Nordic's nRF5340 supports proprietary radio modes alongside BLE.
Requires new hardware (separate RF module), custom protocol development.

**BLE Audio (LC3/LC3plus):**
Bluetooth LE Audio was designed specifically for low-latency audio streaming.
The LC3 codec achieves ~5 ms codec latency. BLE connection events provide
semi-deterministic scheduling (fixed intervals, no CSMA/CA contention).
The RW612 has BLE support. Main limitation: lower throughput than WiFi.

**DECT (Digital Enhanced Cordless Telecommunications):**
TDMA-based protocol on a dedicated 1.9 GHz band (varies by region).
Used by professional wireless microphone systems (Shure, Sennheiser).
Completely deterministic timeslots, ~2-5 ms achievable, dedicated spectrum = no
contention from other devices. Requires a DECT transceiver IC.

---

## Quick Reference: Acronyms

| Acronym | Full Name | Quick Explanation |
|---------|-----------|-------------------|
| AC_VO | Access Category Voice | Highest-priority WMM traffic class |
| AIFSN | Arbitration Inter-Frame Spacing Number | Minimum idle slots before contention starts |
| A-MPDU | Aggregate MAC Protocol Data Unit | Batching multiple frames into one TX |
| AP | Access Point | The WiFi device that creates the network |
| BLE | Bluetooth Low Energy | Low-power Bluetooth variant |
| CCA | Clear Channel Assessment | Radio check: "is someone else transmitting?" |
| CSMA/CA | Carrier Sense Multiple Access / Collision Avoidance | WiFi's "listen before talk" protocol |
| CW | Contention Window | Range for random backoff before transmitting |
| DECT | Digital Enhanced Cordless Telecommunications | Dedicated-band cordless protocol (1.9 GHz) |
| DFS | Dynamic Frequency Selection | Radar avoidance on certain 5 GHz channels |
| DMA | Direct Memory Access | Hardware moves data without CPU involvement |
| DSCP | Differentiated Services Code Point | 6-bit QoS marking in the IP header |
| EDCA | Enhanced Distributed Channel Access | WMM's mechanism for traffic prioritization |
| EDMAC | Energy Detect Multi-Address Correlation | Radar detection feature in some bands |
| EWMA | Exponentially Weighted Moving Average | Simple low-pass filter: `out = a*new + (1-a)*old` |
| FEC | Forward Error Correction | Redundant data that allows recovering lost packets |
| I2S | Inter-IC Sound | Digital audio bus between chips |
| ISR | Interrupt Service Routine | Hardware interrupt handler |
| lwIP | Lightweight IP | Small TCP/IP stack for embedded systems |
| mlan | Marvell LAN | NXP/Marvell WiFi firmware layer |
| OFDMA | Orthogonal Frequency Division Multiple Access | WiFi 6: AP schedules who transmits when |
| PCM | Pulse Code Modulation | Uncompressed digital audio samples |
| PLC | Packet Loss Concealment | Generating replacement audio for lost packets |
| PS | Power Save | WiFi radio sleep mode |
| QoS | Quality of Service | Traffic prioritization mechanisms |
| RTS/CTS | Request To Send / Clear To Send | Channel reservation handshake |
| STA | Station | WiFi client device |
| TB-PPDU | Trigger-Based PPDU | WiFi 6: frame sent in response to AP trigger |
| TDMA | Time Division Multiple Access | Fixed timeslot scheduling (deterministic) |
| TID | Traffic Identifier | Per-packet priority tag (0-7) |
| ToS | Type of Service | IP header byte for QoS marking |
| TWT | Target Wake Time | WiFi 6: scheduled sleep/wake periods |
| UAC2 | USB Audio Class 2 | USB standard for audio devices |
| uAP | Micro Access Point | Software AP mode on a WiFi chip |
| UNII | Unlicensed National Information Infrastructure | 5 GHz regulatory bands |
| WMM | WiFi Multimedia | QoS framework with 4 traffic classes |
