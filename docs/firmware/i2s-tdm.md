# I2S/TDM

`firmware/src/audio/i2s.*` owns the Zephyr I2S/TDM device and presents a FIFO
based PCM transport to the audio path modules.

## Purpose

Move fixed-size PCM blocks between path-owned FIFO objects and the nRF TDM
peripheral.

## Source Files

| File | Role |
|---|---|
| `firmware/src/audio/i2s.c` | I2S thread, Zephyr I2S config, DMA slabs, active/inactive loop. |
| `firmware/src/audio/i2s.h` | I2S activation API; includes the shared PCM constants from `audio_config.h`. |
| `firmware/prj.conf` | Enables I2S and sets TX/RX TDM block counts. |
| `firmware/boards/nrf54lm20dk_nrf54lm20a_cpuapp.overlay` | TDM pins, DMA RAM region, 32 MHz reference clock output. |

## Buffers

| Direction | FIFO contract |
|---|---|
| Speaker TX | Reads complete stereo PCM blocks of `AUDIO_I2S_BLOCK_BYTES` from the path-provided TX FIFO. |
| Mic RX | Writes a mono chunk containing one left-channel sample per stereo frame to the path-provided RX FIFO. |

## Ownership

The I2S thread owns the `tdm` device, TX/RX DMA slabs, command queue, running
flag, and pending-byte counter. Audio path modules own the `tu_fifo_t` objects
passed into `audio_i2s_activate()`.

The FIFO type is always TinyUSB's `tu_fifo_t`, even when the active path is not
using USB audio. In headset mode, USB enumerates CDC-only, but `path_headset`
still defines local static speaker and mic FIFOs with `tu_fifo_config()`. This
lets wired and headset playback use the same I2S buffering contract: the I2S
layer only sees a TX FIFO and an RX FIFO, not whether those FIFOs are USB
endpoint FIFOs or local path buffers.

## PCM Geometry

| Symbol | Meaning |
|---|---|
| `AUDIO_SAMPLE_RATE_HZ` | Fixed PCM frame rate used by USB, I2S, PROP sizing, and codec FLL targeting. |
| `AUDIO_I2S_CHANNELS` | Number of TDM/I2S slots. |
| `AUDIO_BITS_PER_SAMPLE` | Bits per channel sample. |
| `AUDIO_I2S_BYTES_PER_FRAME` | Bytes per interleaved I2S stereo sample pair. |
| `AUDIO_I2S_BYTES_PER_MS` | Derived byte rate for stereo PCM. |
| `AUDIO_I2S_BLOCK_BYTES` | DMA block size and speaker FIFO transfer unit. |
| `AUDIO_I2S_SAMPLE_PAIRS_PER_BLOCK` | Interleaved L/R sample pairs per DMA block. |

## Stereo To Mono

The TDM/codec interface is configured as stereo, but the firmware exports mic
audio as mono. `audio_i2s_rx_block()` calls `extract_left_to_mono()` on every
RX block before writing to the path RX FIFO.

The extraction copies the left channel sample from each interleaved stereo
frame into the front of the same slab block, then writes the resulting mono
chunk to the RX FIFO. The right channel is ignored. Keeping this conversion in
the I2S layer means `path_wired` and `path_headset` both receive the same mono
mic FIFO format; neither path needs to understand the codec stereo slot layout.

## Public API

| Function | Behavior |
|---|---|
| `audio_i2s_activate(tx_fifo, rx_fifo)` | Enqueues an activate command with path-owned FIFOs. |
| `audio_i2s_deactivate()` | Enqueues a deactivate command. |
| `audio_i2s_tx_get_pending_bytes()` | Returns speaker bytes submitted to I2S and not yet matched by RX completion. |

The command queue depth is one. Calls block if the command slot is full.
Current implementation requires both `tx_fifo` and `rx_fifo` to be non-null.

## Thread Startup

`K_THREAD_DEFINE(audio_i2s_thread_id, ...)` starts the I2S thread at boot.

Startup sequence:

1. Verify `tdm` and `i2c23` devices are ready.
2. Initialize the NAU88L21 codec over I2C.
3. Configure Zephyr I2S TX and RX directions.
4. Wait for an activate command.

## Zephyr I2S Config

Both directions use:

| Field | Value |
|---|---|
| `word_size` | `AUDIO_BITS_PER_SAMPLE` |
| `channels` | `AUDIO_I2S_CHANNELS` |
| `format` | `I2S_FMT_DATA_FORMAT_I2S` |
| `frame_clk_freq` | `AUDIO_SAMPLE_RATE_HZ` |
| `block_size` | `AUDIO_I2S_BLOCK_BYTES` |
| `options` | `I2S_OPT_BIT_CLK_SLAVE \| I2S_OPT_FRAME_CLK_SLAVE` |

The I2S config `timeout` field differs by direction:

| Direction | Timeout |
|---|---|
| TX | `0` / no wait |
| RX | `SYS_FOREVER_MS` |

DMA memory comes from static slabs:

| Slab | Block size | Block count |
|---|---:|---:|
| TX | `AUDIO_I2S_BLOCK_BYTES` | `CONFIG_I2S_NRF_TDM_TX_BLOCK_COUNT` |
| RX | `AUDIO_I2S_BLOCK_BYTES` | `CONFIG_I2S_NRF_TDM_RX_BLOCK_COUNT` |

The board overlay reserves a `DMA_RAM` region for TDM and TWIM EasyDMA.

## Active Loop

On activation:

- Pre-fill TX by submitting every full `AUDIO_I2S_BLOCK_BYTES` block currently
  available.
- Trigger `I2S_TRIGGER_START` for both directions.
- Retry start in a bounded prepare/start loop.

While active:

- Check for a queued deactivate command.
- Block on `i2s_read()` for the next RX block.
- Decrement pending TX bytes by one block after RX completion.
- Extract left channel to mono and write it to the RX FIFO.
- Submit all complete `AUDIO_I2S_BLOCK_BYTES` blocks available from the TX FIFO.

On stop or error, the thread issues `I2S_TRIGGER_DROP`, clears the running flag,
and resets pending bytes to zero.

## Pending Byte Accounting

`audio_i2s_tx_pending_bytes` increments by `AUDIO_I2S_BLOCK_BYTES` after each
successful `i2s_write()`. It decrements by the same amount after each
successful `i2s_read()`.

Audio path code adds this value to the speaker FIFO count so buffer control
includes both software-buffered audio and audio already committed to DMA/I2S.

## Codec Clock Relationship

Only codec-master clocking is supported:

- nRF TDM is BCLK/LRCK slave.
- The board drives MCLKI to the NAU88L21 from nRF PCLK.
- NAU88L21 FLL output generates the codec master clocks.
- Audio paths retune the codec FLL, which changes the I2S consume rate.

See [NAU88L21 Codec](nau88l21-codec.md) for clock and FLL details.

## Constraints

- PCM geometry is fixed by compile-time audio constants.
- One active path at a time.
- RX and TX progress are treated as paired for pending-byte accounting.
- Microphone data exported by this layer is left-channel mono only.
