# Audio Path

Audio path code owns path selection behavior after `app_control` chooses a boot
mode. PROP is treated as a packet service here; radio/TDD internals are in
[PROP TDD](prop-tdd.md).

## Purpose

Provide three path implementations over shared buffer-state names, FLL state,
and codec-clock control:

| File | Ownership |
|---|---|
| `firmware/src/audio/path_common.*` | Shared path state names, warning helper, PI controller, global FLL fixed/auto state, PROP audio packet-size symbols. |
| `firmware/src/audio/path_wired.c` | Direct USB audio to codec path. |
| `firmware/src/audio/path_dongle.c` | USB audio to PROP path for the dongle. |
| `firmware/src/audio/path_headset.c` | PROP audio to codec path for the headset. |

## Path Matrix

| Mode | Path | Speaker flow | Mic flow | Buffer/FLL |
|---|---|---|---|---|
| `usb` | `path_wired` | USB EP OUT FIFO -> I2S TX | I2S RX left mono -> USB EP IN FIFO | Uses `BUFFERING` / `PLAYING` and FLL PI control. |
| `prop_dongle` | `path_dongle` | USB EP OUT FIFO -> PROP TX queue | PROP RX queue -> USB EP IN FIFO | No I2S, codec, buffering state, or FLL control. |
| `prop_headset` | `path_headset` | PROP RX queue -> speaker FIFO -> I2S TX | I2S RX left mono -> mic FIFO -> PROP TX queue | Uses `BUFFERING` / `PLAYING` and FLL PI control. |

## Buffer States

Buffer states are only used by the I2S-backed paths: `path_wired` and
`path_headset`. `path_dongle` does not maintain `BUFFERING` / `PLAYING`; it
moves USB and PROP payloads as soon as work is available. If the dongle has a complete USB
speaker payload, it sends it to PROP. If it receives a complete PROP mic packet,
it writes the audio payload back to the USB IN endpoint.

| State | Meaning |
|---|---|
| `PATH_STATE_BUFFERING` | Path is waiting for enough speaker data before activating I2S. |
| `PATH_STATE_PLAYING` | Path has activated I2S and is feeding speaker blocks. |

Wired and headset paths use the same state rule shape: start playback when the
speaker FIFO plus pending I2S bytes reaches the path-local start threshold, and
return to buffering when submitted I2S data has drained and the speaker FIFO no
longer contains a full block. The exact thresholds live next to each path
implementation.

## Inputs and Outputs

| Boundary | Input | Output |
|---|---|---|
| USB audio | TinyUSB EP OUT speaker FIFO | TinyUSB EP IN mic FIFO |
| PROP | `prop_session_rx_dequeue()` | `prop_session_tx_enqueue()` |
| I2S/TDM | TX FIFO supplies speaker data to `audio_i2s_activate()` | RX FIFO receives mic data from I2S |
| CLI/status | `path_*_get_status()` | `#A` status lines from `cli.c` |

## Ownership

| Owner | Resources |
|---|---|
| `path_common` | Shared state names, controller math, packet-size symbols, global fixed/auto FLL state. |
| `path_wired` | One path thread and wired playback status. |
| `path_dongle` | One path thread and dongle overflow counter. |
| `path_headset` | One path thread, local speaker/mic FIFOs, headset playback status. |

## Runtime Behavior

`path_wired`:

- Waits for TinyUSB audio EP OUT and EP IN FIFOs.
- Polls on a short sleep loop.
- Starts I2S when USB speaker level plus I2S pending bytes reaches the wired
  start threshold.
- Stops I2S and increments `spk_underrun_events` when playback drains below one
  I2S block.
- Retunes the codec FLL while playing.

`path_dongle`:

- Waits for TinyUSB audio EP OUT and EP IN FIFOs.
- Does not start or stop a stream state machine.
- Sends complete USB speaker payloads to PROP.
- Writes complete received PROP mic payloads back to the USB IN endpoint.
- Counts speaker bytes consumed from USB after a PROP TX enqueue failure in
  `overflow_bytes`.

`path_headset`:

- Creates local TinyUSB FIFOs for speaker and mic buffering.
- Starts I2S when speaker FIFO plus pending I2S bytes reaches the headset start
  threshold.
- Moves PROP speaker packets into the local speaker FIFO.
- Moves complete mic payloads from the local mic FIFO to PROP TX.
- Retunes the codec FLL while playing.

## FLL Controller

`update_codec_clock()` runs the shared FLL update loop:

| Input | Use |
|---|---|
| `target` | Desired total speaker level in bytes. |
| `fifo` | Current software speaker FIFO level. |
| `pending` | Bytes already submitted to the I2S pipeline. |

It filters `fifo + pending`, computes the buffer error, passes that error to
the internal PI controller, computes
`target_rate = AUDIO_I2S_SAMPLE_RATE_HZ - adjust_hz`, and calls
`nau88l21_set_fll_target_rate_hz()`. `fll_set_fixed()` pauses automatic updates
by setting a global fixed target; `fll_set_auto()` resumes automatic control.

## Constraints

- Audio format is fixed by the I2S/USB constants; it is not negotiated per path.
- Speaker samples are stereo; captured I2S samples are reduced to mono by
  keeping the left channel in the I2S layer.
- The active path is selected only at boot; the CLI persists a new mode and
  reboots.
- The PROP audio path uses fixed packet sizes and no audio-layer stream ID.
- Path modules do not implement packet-loss concealment.
