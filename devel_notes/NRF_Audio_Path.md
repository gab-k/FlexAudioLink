# nRF Audio Path Spec (Implementation-Aligned)

## Status

Implemented audio bridges:

- USB <-> I2S (wired path)
- USB <-> PFSK (wireless path, dongle role)
- PFSK <-> I2S (wireless path, headset role)

Planned, not implemented:

- BLE audio transport
- audio-layer packet loss concealment / PLC
- audio-layer sequence gap accounting

Current implementation constraint:

- much of the audio path is hard-coded for 48 kHz, 16-bit, stereo PCM

## Source of Truth Files

Profile orchestration and route selection:

- `firmware/src/app_control.c`

Shared audio-path helpers/constants:

- `firmware/src/audio/path_common.c`
- `firmware/src/audio/path_common.h`

Route implementations:

- `firmware/src/audio/path_wired.c`
- `firmware/src/audio/path_dongle.c`
- `firmware/src/audio/path_headset.c`

I2S/codec pipeline:

- `firmware/src/audio/i2s.c`
- `firmware/src/audio/i2s.h`

USB audio FIFO adapter:

- `firmware/src/usb/usb_audio.c`
- `firmware/src/usb/usb_audio.h`

## Route Selection and Activation

`app_control_apply()` applies the path implied by the current profile:

- `profile=usb` -> wired path
- `profile=pfsk_dongle` -> wireless path, dongle behavior
- `profile=pfsk_headset` -> wireless path, headset behavior

There is no separate audio-path coordinator or active-path state enum; the active audio path is derived from the `app_control` profile.

Concrete route activation owns cross-route shutdown:

- `audio_path_wired_activate()` deactivates wireless before starting wired.
- `audio_path_wireless_activate()` deactivates wired before starting wireless.

Wireless role behavior is selected from `app_control_get_current_profile()` at runtime; the wireless path does not keep a separate role copy.

### Effective Runtime Matrix

| Profile | Audio path | Data flow |
|---|---|---|
| `usb` | `wired` | USB speaker -> I2S TX, I2S RX -> USB mic |
| `pfsk_dongle` | `wireless` | USB speaker -> PFSK TX, PFSK RX -> USB mic |
| `pfsk_headset` | `wireless` | PFSK RX -> I2S TX, I2S RX -> PFSK TX |

Special case: changing between PFSK profiles resets the wireless path so role-specific behavior updates immediately.

## Audio Units and Constants

From `audio_path_common.h` and `i2s.h`:

- `AUDIO_BYTES_PER_STEREO_SAMPLE = 4`
- `AUDIO_I2S_BYTES_PER_MS = 192` (48 kHz, 16-bit stereo)
- `AUDIO_I2S_BLOCK_BYTES = AUDIO_I2S_BYTES_PER_MS / 2 = 96` (0.5 ms stereo)
- `AUDIO_PFSK_SPK_PACKET_BYTES = 192` (1 ms stereo speaker payload)
- `AUDIO_PFSK_MIC_PACKET_BYTES = 96` (1 ms mono mic payload)
- `AUDIO_DMA_MAX_BYTES = 384`

Watermarks:

- `AUDIO_START_BYTES = 1152` (6 ms)
- `AUDIO_TARGET_BYTES = 960` (5 ms)
- `AUDIO_PANIC_LOW_BYTES = 384` (2 ms)
- `AUDIO_PANIC_HIGH_BYTES = 1728` (9 ms)

Feedback/filter gains:

- `AUDIO_FILTER_ALPHA = 1/20`
- `AUDIO_P_GAIN_DIV = 8`
- `AUDIO_P_ADJUST_MAX_HZ = 200`

## Shared Runtime Rules

State machine (`audio_state_advance`):

- start in `BUFFERING`
- `BUFFERING -> PLAYING` when `level_bytes >= AUDIO_START_BYTES`
- `PLAYING -> BUFFERING` when `level_bytes == 0`

Low panic does not leave `PLAYING`; it drives the feedback panic override and, in the wireless path only, lets route code insert/count silence when there is no full playback block ready. The wired path does not insert silence.

Filter (`audio_filter_update`):

- first sample initializes filter
- then EMA with alpha `1/20`

P-control (`audio_p_controller_step`):

- `error_bytes = AUDIO_TARGET_BYTES - filtered_level_bytes`
- `adjust_hz = error_bytes / 8`
- panic overrides:
- if low panic: `+200 Hz`
- if high panic: `-200 Hz`
- clamp to `[-200, +200]`

Stereo-to-mono extraction (`audio_extract_left_to_mono`):

- capture uses left channel only
- one `AUDIO_I2S_BLOCK_BYTES` stereo block (`96` bytes) -> `48` mono bytes
- two `AUDIO_I2S_BLOCK_BYTES` stereo blocks (`192` bytes) -> one `AUDIO_PFSK_MIC_PACKET_BYTES` mono wireless mic packet

## Wired Path (`audio_path_wired.c`)

Purpose:

- bridge USB Audio endpoints directly with I2S queues
- no extra software playback ring

Activation/reset:

- reset filter/status to buffering
- flush I2S TX/RX queues
- `usb_audio_reset()` clears USB audio endpoint FIFOs and feedback state

Loop (1 ms period):

1. Drain I2S RX blocks -> left-channel mono -> USB mic endpoint FIFO.
2. Read USB speaker FIFO level and update filter/error/stream-state.
3. If `PLAYING`, move `AUDIO_I2S_BLOCK_BYTES` (`96-byte`) chunks USB speaker FIFO -> I2S TX queue.
4. Apply USB feedback (`48kHz + spk_p_adjust_hz`) in 16.16 samples/microframe.
5. Publish USB mic FIFO level into status.

The wired path relies on the I2S driver's empty-TX-queue behavior (zero block
sent) for short-term underruns; there is no explicit silence-insertion logic
and no underrun counter in this path.

Status fields (`audio_path_wired_status`):

- `active`
- `stream_state`
- `spk_level_bytes`
- `spk_filtered_level_bytes`
- `spk_error_bytes`
- `spk_p_adjust_hz`
- `overflow_bytes`
- `mic_level_bytes`

## Wireless Path (`audio_path_wireless.c`)

Purpose:

- profile-dependent bridge using one local playback ring and PFSK session queues

Local ring:

- `WIRELESS_RING_BYTES = 4096`
- TinyUSB FIFO configured in overwrite mode
- `audio_ring_push()` returns evicted-oldest bytes for accounting

PFSK audio payload map:

- session queues `pfsk_packet` directly
- `pfsk_packet.length` is metadata plus payload bytes
- `pfsk_packet.payload` contains only the audio bytes after the 2-byte PFSK metadata
- audio producers add `PFSK_PACKET_METADATA_LEN` when enqueueing frames
- audio consumers subtract `PFSK_PACKET_METADATA_LEN` after dequeueing frames

Direction is implicit from the device role; there is no stream id byte.
Audio byte count is not duplicated in the audio payload.

`peer_meta` layout is direction-dependent:

- Headset -> Dongle: bits `0..11` carry the headset's EMA-filtered ring
  level in bytes (saturated at 4095). Bits `12..15` are reserved and must
  be 0.
- Dongle -> Headset: all 16 bits are currently reserved and must be 0.
  This slot is earmarked for future downlink control signalling (e.g.
  speaker mute, speaker volume) and for a future uplink mic-mute request
  bit carried in the headset->dongle direction.

Feedback flows one way: only headset->dongle frames carry a usable ring
level. The dongle runs its USB feedback P-controller off that value so
the loop closes around the actual consumer (the headset I2S output).
The dongle's own local ring level is only used to gate the internal
`BUFFERING -> PLAYING` transition and is not transmitted. Filtering is
done on the sender side with the shared EMA (`AUDIO_FILTER_ALPHA = 1/20`),
so the receiver uses the value directly with no further smoothing.

### Dongle role behavior

- ingest USB speaker bytes (up to `AUDIO_DMA_MAX_BYTES=384` per pull) into ring
- parse received capture frames, push audio to USB mic FIFO, latch peer ring level from `peer_meta`
- once `PLAYING`, send playback from ring to PFSK in `AUDIO_PFSK_SPK_PACKET_BYTES` chunks; outbound `peer_meta` is zero (reserved)
- update USB feedback from the latched peer (headset) ring level

### Headset role behavior

- parse received playback frames, push audio to local ring (inbound `peer_meta` is currently unused)
- send I2S capture to PFSK as mono; outbound `peer_meta` carries the headset's filtered ring level
- once `PLAYING`, send playback from ring to I2S in `AUDIO_I2S_BLOCK_BYTES` (`96-byte`) blocks
- if low panic, inject one silence I2S block and count underrun/silence bytes
- no USB feedback update path in this role

### PFSK test-mode interaction

When `pfsk_test_mode_is_running()`:

- ring is cleared each step
- status is forced to buffering/zeroed level+error+adjust
- normal audio bridging work is skipped

Status fields (`audio_path_wireless_status`):

- `active`
- `stream_state`
- `spk_level_bytes` (local ring)
- `spk_underrun_bytes`
- `overflow_bytes`
- `spk_silence_inserted_bytes`
- `spk_dropped_oldest_bytes`
- `spk_usb_level_bytes`
- `mic_usb_level_bytes`
- `peer_ring_level_bytes` (sender-filtered level decoded from last peer_meta)
- `peer_ring_error_bytes` (`AUDIO_TARGET_BYTES - peer_ring_level_bytes`)
- `spk_p_adjust_hz` (USB feedback adjust, dongle-only nonzero)
- `rx_malformed_frames`

## USB Audio Adapter (`usb_audio.c`)

The audio path reads/writes TinyUSB endpoint FIFOs directly:

- speaker source: `tud_audio_get_ep_out_ff()`
- microphone sink: `tud_audio_get_ep_in_ff()`

`usb_audio_reset()`:

- restores nominal feedback (`48000/8000` in 16.16)
- clears EP OUT and EP IN FIFOs

Descriptor/channel configuration (`tusb_config.h` + descriptors):

- speaker (OUT): 48 kHz, 16-bit, stereo
- microphone (IN): 48 kHz, 16-bit, mono

## I2S Contract (`i2s.c/.h`)

- `AUDIO_I2S_SAMPLE_RATE_HZ = 48000`
- `AUDIO_I2S_CHANNELS = 2`
- `AUDIO_I2S_BLOCK_BYTES = AUDIO_I2S_BYTES_PER_MS / 2 = 96` (0.5 ms)
- TX/RX msg queues depth: `12`

Runtime behavior:

- I2S thread continuously submits TX and collects RX blocks
- if TX queue is empty, zero block is sent
- RX queue overflow policy drops oldest block to keep newest data

## CLI Audio Status Surface

Current command:

- `status_audio on [ms]`
- `status_audio off`

Output is periodic `#A ...` lines while enabled.

Formats:

- wired: includes level/filter/error/P-adjust/overrun/USB mic level
- wireless: wired-style `spk_*` fields plus `spk_dropped_oldest_bytes`, `spk_usb_level_bytes`, and `mic_usb_level_bytes`
- none: `#A active=none`

There is currently no dedicated one-shot `status_audio` dump command.

## Transition and Flush Rules

On profile changes, `app_control` activates the route implied by the new profile. Concrete route activation deactivates the opposing route first.

Reset/deactivate paths flush local transient state:

- wired: clear status, flush I2S queues
- wireless: clear status, clear ring, flush I2S queues

`usb_audio_reset()` is part of route activation reset for both wired and wireless paths.
