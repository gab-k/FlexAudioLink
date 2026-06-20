# Audio Path

Audio path code owns the PCM routing selected by `app_control` at boot. PROP is
treated as a packet service here; radio/TDD internals are covered in
[PROP TDD](prop-tdd.md), and codec-facing PCM transport is covered in
[I2S/TDM](i2s-tdm.md).

## Source Files

| File | Ownership |
|---|---|
| `firmware/src/app_control.c` | Selects the boot mode and initializes the matching path. |
| `firmware/src/audio/path_common.*` | Shared path state names, level filter, PI controller, global fixed/auto FLL state, and PROP audio packet-size symbols. |
| `firmware/src/audio/path_wired.c` | Direct USB audio to codec path. |
| `firmware/src/audio/path_dongle.c` | USB audio to PROP path for the dongle. |
| `firmware/src/audio/path_headset.c` | PROP audio to codec path for the headset. |
| `firmware/src/usb/usb_audio.*` | TinyUSB audio FIFO adapter and audio reset helper. |
| `firmware/src/audio/i2s.*` | I2S/TDM activation API and codec-facing transport thread. |

## Path Matrix

| Mode | Path | Speaker flow | Mic flow | Buffer/FLL |
|---|---|---|---|---|
| `usb` | `path_wired` | USB EP OUT FIFO -> I2S TX | I2S RX left mono -> USB EP IN FIFO | Uses `BUFFERING` / `PLAYING` and FLL PI control. |
| `prop_dongle` | `path_dongle` | USB EP OUT FIFO -> PROP TX queue | PROP RX queue -> USB EP IN FIFO | No I2S, codec, buffering state, or FLL control. |
| `prop_headset` | `path_headset` | PROP RX queue -> speaker FIFO -> I2S TX | I2S RX left mono -> mic FIFO -> PROP TX queue | Uses `BUFFERING` / `PLAYING` and FLL PI control. |

The active path is derived from the persisted `app_control` mode. The CLI
`set mode <usb|prop_dongle|prop_headset>` command saves the new mode and
reboots; paths are not hot-swapped without reboot.

## Audio Units

The fixed PCM geometry lives in `firmware/src/audio/audio_config.h`.
Speaker audio is 48 kHz, 16-bit, stereo PCM; microphone audio is 48 kHz,
16-bit, mono PCM after the I2S layer extracts the left channel.

Important derived units:

| Symbol | Meaning |
|---|---|
| `AUDIO_SPK_TARGET_BYTES` | Target speaker buffer level used to start I2S-backed paths and close the FLL loop. |
| `AUDIO_SPK_BUFFER_BYTES` | Local headset speaker FIFO capacity. |
| `AUDIO_MIC_BUFFER_BYTES` | Local headset mic FIFO capacity. |
| `AUDIO_I2S_BLOCK_BYTES` | I2S DMA block size and speaker transfer unit. |
| `PROP_SPK_PACKET_BYTES` | One PROP speaker payload. |
| `PROP_MIC_PACKET_BYTES` | One PROP mic payload. |

## Buffer States

Buffer states are only used by the I2S-backed paths: `path_wired` and
`path_headset`. `path_dongle` does not maintain `BUFFERING` / `PLAYING`; it
moves USB and PROP payloads whenever complete payloads are available.

| State | Meaning |
|---|---|
| `PATH_STATE_BUFFERING` | Path is waiting for enough speaker data before activating I2S. |
| `PATH_STATE_PLAYING` | Path has activated I2S and is feeding speaker blocks. |

Wired and headset paths use the same state rule shape: start playback when the
speaker FIFO plus pending I2S bytes reaches `AUDIO_SPK_TARGET_BYTES`, and return
to buffering when pending I2S data has drained and the speaker FIFO has less
than one full `AUDIO_I2S_BLOCK_BYTES` block. Returning to buffering increments
`spk_underrun_events`.

## Runtime Behavior

`path_wired`:

- Waits for TinyUSB audio EP OUT and EP IN FIFOs.
- Uses the USB speaker FIFO directly as the I2S TX source.
- Starts I2S when USB speaker level plus I2S pending bytes reaches
  `AUDIO_SPK_TARGET_BYTES`.
- Stops I2S and increments `spk_underrun_events` when playback drains below one
  I2S block.
- Retunes the codec FLL while playing.

`path_dongle`:

- Waits for TinyUSB audio EP OUT and EP IN FIFOs.
- Reads USB speaker data in `PROP_SPK_PACKET_BYTES` chunks and enqueues PROP TX
  packets.
- Dequeues PROP RX packets with `PROP_MIC_PACKET_BYTES` payloads and writes them
  to the USB mic FIFO.
- Counts speaker bytes consumed from USB after a PROP TX enqueue failure in
  `overflow_bytes`.

`path_headset`:

- Creates local TinyUSB-style speaker and mic FIFOs with `tu_fifo_config()`.
- Moves PROP speaker packets into the local speaker FIFO.
- Starts I2S when speaker FIFO plus pending I2S bytes reaches
  `AUDIO_SPK_TARGET_BYTES`.
- Sends complete mic payloads from the local mic FIFO to PROP TX.
- Retunes the codec FLL while playing.

## PROP Audio Payloads

Audio paths enqueue and dequeue `struct prop_packet` objects through the PROP
session API. `prop_packet.length` includes `PROP_PACKET_METADATA_LEN`; audio
payload size is `packet.length - PROP_PACKET_METADATA_LEN`.

| Direction | Payload size |
|---|---|
| Dongle -> Headset speaker | `PROP_SPK_PACKET_BYTES` |
| Headset -> Dongle mic | `PROP_MIC_PACKET_BYTES` |

The audio payload does not carry a stream ID or duplicate byte count. Direction
is implied by the active device role.

## FLL Controller

The I2S-backed path threads monitor speaker level while playing and run the FLL
controller when automatic FLL mode is active:

| Input | Use |
|---|---|
| Path target | Desired total speaker level in bytes. |
| Software FIFO level | Current path-owned speaker FIFO level. |
| I2S pending level | Bytes submitted to the I2S pipeline and not yet matched by RX completion. |

Each path periodically filters `fifo + pending` with
`codec_level_filter_update()`. On the FLL update interval, it passes
`AUDIO_SPK_TARGET_BYTES` and `AUDIO_SAMPLE_RATE_HZ` to
`codec_clock_controller()`. The controller computes buffer error, derives
warning levels from the target, applies PI control, and calls
`nau88l21_set_fll_target_rate_hz()` with `AUDIO_SAMPLE_RATE_HZ - adjust_hz`.

`fll_set_fixed()` pauses automatic updates by setting a global fixed target;
`fll_set_auto()` resumes automatic control. FLL control is unavailable in
`prop_dongle` mode because that path does not use the codec or I2S.

## USB Audio Adapter

Audio paths read and write TinyUSB endpoint FIFOs directly:

| Direction | FIFO accessor |
|---|---|
| Speaker source | `tud_audio_get_ep_out_ff()` |
| Microphone sink | `tud_audio_get_ep_in_ff()` |

`usb_audio_reset()` restores nominal feedback and clears the audio endpoint
FIFOs. USB audio callbacks wake `path_wired` or `path_dongle` depending on the
current mode.

## CLI Status

`status_audio on [ms]` enables periodic `#A ...` lines. `status_audio off`
disables them.

`usb` and `prop_headset` report the shared codec path status:

- `state`
- `spk_fifo_bytes`
- `spk_pending_bytes`
- `spk_filtered_level_bytes`
- `spk_error_bytes`
- `spk_p_adjust_hz`
- `spk_i_adjust_hz`
- `spk_fll_target_rate_hz`
- `spk_underruns`

`prop_dongle` reports:

- `overflow_bytes`

## Constraints

- Audio format is fixed by compile-time constants; it is not negotiated per
  path.
- Speaker samples are stereo; captured I2S samples are reduced to mono by
  keeping the left channel in the I2S layer.
- The active path is selected at boot from persisted mode.
- PROP audio uses fixed speaker and mic packet sizes.
- Path modules do not implement packet-loss concealment.
