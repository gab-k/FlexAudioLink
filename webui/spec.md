# Wireless Headset Configuration GUI — Design Specification

## Overview

This document describes the design and feature requirements for the web-based configuration GUI for the wireless headset + dongle system. The GUI communicates with the dongle over **USB CDC ACM** (serial) and is the primary interface for end-user configuration, advanced tuning, and firmware updates.

The GUI is delivered as a **hosted web app** (e.g. GitHub Pages) requiring zero installation for most users. It communicates with the dongle via **WebSerial** in Chromium-based browsers, or via a **local WebSocket bridge** for Firefox/Safari users. The actual UI is identical regardless of transport.

---

## Delivery Architecture

### Primary: Hosted Web App (Zero Install)

The GUI is hosted statically on **GitHub Pages** and accessed via a URL. No installation, no download, always up to date.

- Works out of the box in **Chrome, Edge, and other Chromium-based browsers** via the WebSerial API
- User clicks the link, grants serial port permission once, done
- Covers approximately 70% of desktop users

### Fallback: Local WebSocket Bridge (Firefox / Safari)

For browsers that do not support WebSerial (Firefox has no plans to implement it), a minimal companion script bridges the gap.

The bridge is a **single Python file**, runnable with no dependencies beyond PySerial and websockets:

```
pip install pyserial websockets
python websocket_serial_bridge.py
```

Once running, the web app detects WebSerial is unavailable and offers a **"Download Bridge"** button with instructions. The app then connects to `ws://localhost:8765` instead of using WebSerial directly. The protocol is identical either way — the web app does not need to know which transport it is using.

```
Chrome/Edge user:    browser ──WebSerial──────────────────── dongle USB
Firefox user:        browser ──WebSocket──┐
                                          └─ bridge.py ──── dongle USB
```

The bridge script should:
- Auto-detect the dongle by USB VID/PID
- Accept one WebSocket client at a time
- Pass text lines transparently in both directions
- Handle reconnection gracefully if the dongle is unplugged

### Frontend Stack

- **Svelte** — reactive UI with minimal boilerplate, well suited for live-updating meters and graphs
- **uPlot** — high-performance live charting for RSSI, packet loss, and throughput graphs
- **No backend server required** — fully static, GitHub Pages compatible

### Transport Abstraction

The web app should implement a simple transport interface so the rest of the code does not care whether it is talking WebSerial or WebSocket:

```javascript
// transport.js — either backend satisfies this interface
transport.send(textLine)     // sends a plain text string + newline
transport.onMessage(callback) // callback receives raw text lines
transport.connect()
transport.disconnect()
```

On page load, the app checks `navigator.serial` availability and selects the appropriate transport automatically. If neither is available, it shows a clear unsupported browser message with a link to the bridge script.

---

## Architecture Assumptions

- The dongle and headset are **identical hardware** running the **same firmware** — the role is determined entirely by the boot mode setting
- Each device has a **USB-C connector** for firmware updates, configuration, and USB audio mode
- The connected device communicates with the GUI via USB CDC ACM
- Firmware is flashed via **nrfutil** over USB serial (DFU bootloader mode); the GUI cannot flash directly but can trigger DFU mode
- Three boot modes are available: **USB Audio**, **PFSK Dongle**, and **PFSK Headset**
- The proprietary 2.4 GHz link uses **FHSS + TDMA** with no retransmission — missed packets are concealed via **PLC (Packet Loss Concealment)**
- USB mode turns the device into a wired USB Audio Class device — radio and codec settings do not apply
- The GUI communicates over a simple text-based CLI protocol (newline-delimited commands and responses)

---

## GUI Layout

Suggested tab or panel structure:

```
[ Mode ]  [ Status & Monitor ]  [ Audio Settings ]  [ Radio Settings ]  [ Device Settings ]  [ Firmware Update ]  [ Advanced ]
```

---

## 0. Mode Panel

Configures the fundamental operating mode of the connected device. This panel is shown first as it determines the behavior and available options of all other panels.

| Mode | Description | Effect on Other Panels |
|---|---|---|
| `usb` | Wired USB Audio Class device | Radio panel not applicable, codec settings not applicable |
| `pfsk_dongle` | PC-side proprietary 2.4 GHz bridge | Full access to applicable Audio and Radio settings |
| `pfsk_headset` | Headset-side proprietary 2.4 GHz bridge | Full access to applicable Audio and Radio settings |

**Notes:**
- Changing boot mode requires a device restart — warn the user before applying
- Both dongle and headset use the same hardware — the role is part of the persisted firmware mode

---

## 1. Status & Monitor Panel

Read-only live information. Updated on a polling interval (e.g. every 500ms).

### Device Info
- Firmware version (e.g. `v1.2.3`) — same firmware runs on both dongle and headset, role is determined by mode setting
- Peer connected (wireless): Yes / No — whether the other device in the pair is connected

### Battery
- Battery level (percentage)
- Visual battery bar indicator
- Low battery warning threshold (configurable in Device Settings)

### Link Quality
- RSSI (dBm) — live value
- Packet loss % — rolling average over last N seconds (e.g. 5s window)
- Link quality history graph — scrolling chart showing RSSI and packet loss over time (last ~60s)
- TX power currently in use (dBm)

---

## 2. Audio Settings Panel

### Speaker (Headset Output)

| Parameter | Options | Notes |
|---|---|---|
| Sample Rate | 8000, 16000, 24000, 48000, 96000 Hz | Warn if rate strains link budget |
| Bit Width | 8, 16, 24, 32 bit | Configured independently per direction. ADPCM encodes 16-bit samples only and works best up to 48 kHz. (i) tooltip shown always, with ADPCM note appended when ADPCM selected |
| Channels | Mono, Stereo | |
| Codec | PCM (uncompressed), ADPCM, LC3, Opus | See codec notes below |
| Volume | 0–100% slider | Soft volume on headset DSP |
| EQ | 5-band parametric or graphic EQ | Gain per band in dB, ±12dB range |
| Sidetone Level | 0–100% slider (0 = off) | Mic monitoring in ear, latency-critical |

### Microphone (Headset Input → PC)

| Parameter | Options | Notes |
|---|---|---|
| Sample Rate | 8000, 16000, 24000, 48000, 96000 Hz | Voice use cases rarely need >48kHz but 96kHz available for high-fidelity capture |
| Bit Width | 8, 16, 24 bit | Configured independently per direction. (i) tooltip shown always, with ADPCM note appended when ADPCM selected |
| Channels | Mono, Stereo | Stereo mic is niche but include for completeness |
| Codec | PCM, ADPCM, LC3, Opus | Should match or be independently selectable |
| Mic Gain | dB slider or stepped (e.g. 0, 6, 12, 18, 24 dB) | |
| Mic Level Meter | Live VU meter (peak + RMS) | Useful for gain staging |
| Mic Mute | Toggle button | Hardware mute preferred if supported |

### Codec Notes

| Codec | Compression Ratio | Latency Added | Quality | Notes |
|---|---|---|---|---|
| PCM | 1:1 (uncompressed) | 0ms | Perfect | Only viable at lower sample rates / mono on 4Mbps PHY |
| IMA ADPCM | 4:1 | ~0ms | Good (~78dB DR) | Recommended default, trivial complexity |
| LC3 | ~8:1 | 2.5–10ms | Excellent | Good compressed-audio candidate, Nordic SDK supported |
| Opus | Variable | 2.5–20ms | Excellent | Heavier CPU, better for music quality |

**Codec constraints enforced by the GUI:**
- ADPCM: disable bit width options other than 16-bit, cap sample rates at 48 kHz max; (i) tooltip: "ADPCM encodes 16-bit samples only and works best up to 48 kHz. Higher sample rates and other bit depths are not supported."
- LC3: sample rate locked to LC3-supported rates (8k, 16k, 24k, 32k, 48k Hz)
- PCM at 48kHz stereo 16-bit: show red budget warning immediately

### Link Budget Calculator

Displayed prominently below audio settings. Updates in real time as parameters change.

```
Speaker:  48000 Hz × 16 bit × 2 ch ÷ 4 (ADPCM) =  384 kbps
Mic:      48000 Hz × 16 bit × 1 ch ÷ 4 (ADPCM) =  192 kbps
Overhead: (framing, headers, FHSS margin)        =  ~10%
─────────────────────────────────────────────────────────
Total:                                             ~634 kbps
PHY Budget (4Mbps proprietary):                   ~3800 kbps
Headroom:                                          3166 kbps (83%)  ✅
```

- Speaker and mic share the same link pool — the calculator must treat them as a combined load
- Overhead estimate should be conservative (~10–15% for TDMA framing)
- Show a warning banner if headroom < 20%
- Show a hard error and disable "Apply" if calculated load exceeds PHY budget

### TDMA Slot Allocation

Displayed as a visual bar showing how the TDMA frame is divided between downlink (speaker) and uplink (mic). Slot allocation is **dynamic** — proportional to the bitrate ratio of speaker vs mic streams.

- TDMA frame period scales with PHY rate: 2.5ms @ 4Mbps, 5ms @ 2Mbps, 10ms @ 1Mbps
- Slot split is shown as a percentage and in milliseconds
- Updates in real time as audio settings change

### Latency Estimate

Displayed below the TDMA visualization. Shows separate one-way estimates for speaker path (PC → headset ear) and mic path (headset mic → PC). The latency model has two components:

1. **Codec frame accumulation** — zero for PCM/ADPCM (sample-level), 10ms for LC3/Opus (must buffer one full codec frame before encoding)
2. **Jitter buffer** — absorbs TDMA timing variation on the receiver side, depth is **user-configurable** in Radio Settings

Encode/decode processing time is not modeled — it is codec- and hardware-dependent and should be measured on actual hardware.

Each path is shown as a **color-coded pipeline bar** with proportional segment widths. Totals are color-coded: green (<8ms), blue (<20ms), yellow (<35ms), red (>35ms).

**Jitter buffer placement:**
- Speaker path: jitter buffer on the **headset** (receives DL data, buffers before DAC playout)
- Mic path: jitter buffer on the **dongle** (receives UL data, buffers before USB delivery)
- Capacity: configurable 1–50 ms (default: 10 ms)
- **Minimum capacity is clamped to the codec frame size** — the jitter buffer must hold at least one complete codec frame to be decodable. For LC3/Opus (10 ms frames), the minimum jitter buffer is 10 ms. For PCM/ADPCM (sample-level), the minimum is 1 ms.
- **Latency contribution = half the buffer capacity** — the target fill state is always 50% of the buffer. This is the steady-state operating point where there is equal margin to absorb both early and late packets.

**Codec frame accumulation:**

| Codec | Frame accumulation | Min jitter buffer |
|---|---|---|
| PCM | 0 ms | 1 ms |
| ADPCM | 0 ms | 1 ms |
| LC3 | 10 ms | 10 ms |
| Opus | 10 ms | 10 ms |

**Example estimates (frame accumulation + jitter buffer @ 10 ms capacity):**
- PCM/ADPCM @ 10 ms buffer: ~5 ms one-way (jitter buffer latency only: 10/2 = 5 ms)
- LC3/Opus @ 20 ms buffer: ~20 ms one-way (10 ms codec frame + 20/2 = 10 ms jitter buffer)

---

## 3. Radio Settings Panel

The Radio Settings panel includes a **live Link Budget + Latency Estimate** (same component as the Audio panel) so the user can immediately see throughput and latency implications of radio changes (PHY rate, jitter buffer, payload sizes) without switching tabs.

The operating mode (`usb` / `pfsk_dongle` / `pfsk_headset`) is selected in the **Mode panel** — the Radio panel shows the relevant settings for the currently active mode. In USB mode, a message indicates that radio settings do not apply.

### Proprietary 2.4 GHz Options

| Parameter | Options | Notes |
|---|---|---|
| PHY Data Rate | 1 Mbps, 2 Mbps, 4 Mbps | Higher = more bandwidth, potentially less range |
| TX Power | -20, -16, -12, -8, -4, 0, +4, +8 dBm | Tradeoff with battery life |
| DL Payload Size | 0.5–50 ms of audio (slider) | Speaker downlink payload in ms. GUI shows computed byte count. Max 252 bytes per packet (nRF54LM20 hw limit). Warns if ms exceeds byte limit for current audio config |
| UL Payload Size | 0.5–50 ms of audio (slider) | Mic uplink payload in ms. Independent from DL — mic and speaker streams may have very different data rates |
| Jitter Buffer | 1–50 ms of audio (slider, default 10 ms) | Receiver-side buffer capacity in ms. Latency contribution = **half the buffer capacity** (target fill state = 50%). Lower = less latency, higher = more robust |
| FHSS Channel Exclusion Map | Checkboxes or range selector per channel | Block persistent interference sources (e.g. specific WiFi channels) |
| FHSS Sequence | Auto (default) / Manual seed | Manual for advanced users |

**Notes:**
- Both payload size and jitter buffer are specified in **milliseconds of audio**, making them independent of sample rate / bit width / codec changes. The GUI computes and displays the resulting byte count for payload size.
- **Payload size is clamped to the jitter buffer capacity** — a payload larger than the buffer cannot be absorbed without overflow, causing guaranteed packet loss. The jitter buffer slider is placed above the payload sliders, and reducing the jitter buffer auto-clamps both DL/UL payloads. (i) tooltips explain this constraint on both payload sliders.
- Switching PHY data rate updates the link budget ceiling dynamically. PHY rate changes require reconnection — warn the user.
- Payload byte limit is constrained by the nRF54LM20 radio hardware: the RADIO LENGTH field is 8 bits, giving a max combined S0+LENGTH+S1+PAYLOAD of 258 bytes. With 2-byte sequence header and packet framing, the usable payload max is **252 bytes**.

## 4. Device Settings Panel

### Device Behavior
- Auto-sleep timeout (minutes): `Off / 5 / 10 / 15 / 30 / 60`
- Low battery warning threshold: `5% / 10% / 15% / 20%`

### Audio I/O
- Audio Interface: `wired` / `usb` / `codec`
- Default is set by firmware based on mode (`wired` for USB, `usb` for PFSK dongle, `codec` for PFSK headset)
- Setting both devices to CODEC enables analog-to-analog wireless bridging (e.g. airplane 3.5mm jack → wireless → headset)

### Radio Addressing (Proprietary 2.4 GHz)
- Device Address — this device's radio address (read from device on connect, user-editable)
- Peer Address — the address of the other device in the pair (read from device on connect, user-editable)
- Default addresses are set by firmware; the GUI reads them via `get all` on connect
- Only relevant in PFSK modes; USB has no radio addressing
- For multiple pairs in the same room, configure unique address pairs to avoid crosstalk

### Persistence
- "Save to device" button — writes current config to NVM on both headset and dongle
- "Load from device" button — reads current config from device
- "Export config to file" — saves a JSON snapshot of all settings locally
- "Import config from file" — loads and applies a previously saved JSON config

---

## 5. Firmware Update Panel

Both devices have **USB-C connectors** and are flashed directly over USB. There is no wireless OTA update path.

### Update Flow
- Current firmware version displayed (from `#S fw=...` status push)
- "Enter DFU Mode" button — sends `reset dfu` to put the device into DFU bootloader
- Step-by-step instructions with copy-pasteable `nrfutil dfu usb-serial` command
- After flashing, device reboots automatically; user reconnects from GUI
- Both dongle and headset use the same firmware — to update the other device, connect it via USB-C and repeat

---

## 6. Advanced Panel

> Intended for developers and power users. Consider hiding behind a toggle or separate menu.

### PHY / Protocol Tuning
- TDMA slot timing (if configurable in firmware)
- PLC algorithm selection (if multiple are implemented — e.g. simple zero-fill vs interpolation)
- Packet size / frame duration

### Debug / Diagnostics
- Raw serial console — read-only log output from dongle/headset via CDC ACM
- Send raw command to device (text field + send button)
- Export diagnostic log to file
- Live statistics table:
  - Packets sent / received / lost (cumulative + per second)
  - RSSI histogram
  - Codec frame errors
  - Buffer underruns / overruns

### Developer Shortcuts
- Trigger headset reboot
- Trigger dongle reboot
- Enter DFU mode manually (without going through Firmware Update panel)
- Read device info dump (chip ID, MAC, build flags)

---

## GUI UX Rules

1. **Always show link budget** — it should be visible any time audio settings are on screen, not buried
2. **Greyed-out invalid options** — don't just warn, actively disable incompatible parameter combinations
3. **Apply vs Auto-apply** — audio parameter changes should require an explicit "Apply" button to avoid mid-stream glitches
4. **Unsaved changes indicator** — show a dot or asterisk in the tab title if there are unapplied or unsaved changes
5. **Connection loss handling** — if dongle disconnects mid-session, show a clear reconnect banner without crashing
6. **Mode indicator** — clearly show when USB mode disables radio controls
7. **No silent failures** — every command sent to the device should have a visible success/failure response in a status bar

---

## Serial Protocol (CDC ACM) — Text-Based CLI

The device exposes a human-readable CLI over USB CDC ACM. The same commands work identically from a terminal emulator (minicom/screen) and from the web GUI. All commands and responses are **newline-terminated plain text**.

### Commands (Host → Device)

```
get <param>              # get single param: get volume
get <group>              # get group: get audio, get radio, get device, get mode
get all                  # get everything
set <param> <value>      # set volume 80, set codec_spk adpcm
status                   # one-shot status snapshot
status on [<ms>]         # start periodic status push (default 500ms)
status off               # stop periodic push
echo on|off              # enable/disable local echo (GUI sends "echo off" on connect)
help, reset, scan, linktest  # existing firmware CLI commands, unchanged
```

Commands are **case-insensitive**. The `target` concept (dongle/headset) is gone — the firmware knows which subsystem owns each parameter.

### Responses (Device → Host)

| Pattern | Meaning | Example |
|---|---|---|
| `OK param=value` | Set succeeded | `OK volume=80` |
| `ERR param reason` | Set failed | `ERR codec_spk invalid value "mp3"` |
| `key=value` | Get response (one per line) | `volume=80` |
| `[groupname]` | Group header (human readability aid, GUI ignores) | `[audio]` |
| `#S key=val key=val ...` | Periodic status push (space-separated) | `#S rssi=-62 bat=87 loss=0.1 conn=yes` |
| anything else | Raw console text | Passed to debug console in Advanced panel |

### Status Push Abbreviated Keys

| Key | Meaning |
|---|---|
| `rssi` | RSSI in dBm |
| `bat` | Battery percentage |
| `loss` | Packet loss percentage |
| `conn` | Peer connected (yes/no) |
| `tx` | Packets transmitted (cumulative) |
| `rx` | Packets received (cumulative) |
| `lost` | Packets lost (cumulative) |
| `urun` | Buffer underruns |
| `orun` | Buffer overruns |
| `cerr` | Codec errors |
| `fw` | Firmware version |

### Parameter Table

| Parameter | Group | Type | Values |
|---|---|---|---|
| `sample_rate_spk` | audio | int | 8000..96000 |
| `bit_width_spk` | audio | int | 8,16,24,32 |
| `channels_spk` | audio | string | mono,stereo |
| `codec_spk` | audio | string | pcm,adpcm,lc3,opus |
| `volume` | audio | int | 0..100 |
| `sidetone` | audio | int | 0..100 |
| `sample_rate_mic` | audio | int | 8000..96000 |
| `bit_width_mic` | audio | int | 8,16,24 |
| `channels_mic` | audio | string | mono,stereo |
| `codec_mic` | audio | string | pcm,adpcm,lc3,opus |
| `mic_gain` | audio | int | 0..60 |
| `mic_mute` | audio | bool | on/off |
| `eq` | audio | special | `set eq <band> <freq> <gain>`, get returns `eq0=100,0` etc. |
| `phy_rate` | radio | int | 1,2,4 |
| `tx_power` | radio | int | -20..8 |
| `fhss_exclusion` | radio | list | comma-separated ints or `none` |
| `payload_ms_dl` | radio | float | 0.5..50 |
| `payload_ms_ul` | radio | float | 0.5..50 |
| `jitter_buffer_ms` | radio | float | 1..100 |
| `audio_io` | device | string | wired,usb,codec |
| `device_addr` | radio | hex | 4-byte address, e.g. `0xD0D0D0D0` |
| `peer_addr` | radio | hex | 4-byte address of peer device |
| `mode` | mode | string | usb,pfsk_dongle,pfsk_headset |
| `auto_sleep` | device | int | 0..60 |
| `low_battery_threshold` | device | int | 0..100 |

### GUI Connect Sequence

On successful serial/WebSocket connection, the GUI sends:

```
echo off
status on 500
get all
```

This suppresses local echo, starts periodic status pushes at 500ms, and requests the full current configuration.

---

## Out of Scope for v1

- Mobile app / BLE configuration (considered v2)
- Cloud firmware hosting / auto-update checking
- Multi-device management (more than one headset)
- Per-application audio modes (OS-level integration)
- Noise cancellation controls (requires DSP firmware support)
- Native desktop app (Tauri/Electron) — the web app + bridge covers all use cases without the complexity

---

## Open Questions / To Be Decided in Firmware

- Which PLC algorithm(s) will be implemented?
- Is sidetone processed on the headset CODEC or in firmware?
- Are EQ coefficients computed on-device or sent as pre-calculated biquad coefficients from the GUI?
- What is the exact TDMA frame duration and how does that map to minimum/maximum configurable latency?
- Is there a hardware mute button on the headset, and does it report state back over the wireless link?
