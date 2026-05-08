# Firmware Overview

FlexAudioLink firmware is a Zephyr application for
`nrf54lm20dk/nrf54lm20a/cpuapp`. `main.c` only enters `app_control_boot()`;
long-running work is handled by subsystem threads such as USB, CLI, I2S, PROP,
and the active audio path.

## Module Map

| Module | Purpose | Source files |
|---|---|---|
| `app_control` | Loads persisted boot mode, starts the selected path, and gates USB startup. | `firmware/src/app_control.c`, `firmware/src/main.c` |
| Audio path | Mode-specific audio routing and buffer control. | `firmware/src/audio/path_common.*`, `path_wired.c`, `path_dongle.c`, `path_headset.c` |
| I2S/TDM | Codec-facing PCM transport thread and Zephyr I2S driver owner. | `firmware/src/audio/i2s.*` |
| NAU88L21 | I2C codec init, clock-master setup, headphone DAC bring-up, FLL retune API. | `firmware/src/audio/nau88l21.*` |
| TinyUSB device | USBHS startup, TinyUSB task loop, and mode-dependent descriptors for UAC+CDC or CDC-only enumeration. | `firmware/src/usb/usb_device.c`, `usb_descriptors.c`, `tusb_config.h` |
| USB audio | UAC endpoint FIFOs and UAC control requests. | `firmware/src/usb/usb_audio.c`, `usb_audio.h` |
| CDC CLI | USB CDC transport, command parser, and status emitter. | `firmware/src/usb/usb_cdc.c`, `firmware/src/cli.c` |
| PROP subsystem | Proprietary radio session queues and radio/TDD engine. Treat as one subsystem from audio code. | `firmware/src/prop/session.*`, `radio_core.*` |

See [PROP TDD](prop-tdd.md) for radio turn timing, packet ownership, and
`session.c` / `radio_core.c` coupling.

## Boot Modes

`app_control_boot()` initializes Zephyr settings, reads `mode/active`, defaults
to `usb`, and starts one route:

| Mode | Started modules | Audio route |
|---|---|---|
| `usb` | `path_wired_init()` | USB speaker -> I2S/TDM -> codec, codec RX -> USB mic |
| `prop_dongle` | `prop_session_start_dongle()`, `path_dongle_init()` | USB speaker -> PROP, PROP -> USB mic |
| `prop_headset` | `prop_session_start_headset()`, `path_headset_init()` | PROP -> I2S/TDM -> codec, codec RX -> PROP |

The USB device thread waits on `app_control_await_boot()` before TinyUSB
initialization. Mode changes are persisted by the CLI and require reboot.

## Runtime Ownership

| Owner | Runtime resources |
|---|---|
| `app_control` | Current mode enum and settings key `mode/active`. |
| Audio path threads | Route-local state, TinyUSB FIFO access, PROP session queue use, buffer/FLL status. |
| I2S thread | `tdm` device, TX/RX DMA slabs, I2S command queue, pending-byte counter. |
| NAU88L21 driver | `i2c23` device pointer after init and FLL fractional register writes. |
| USB device thread | USBHS low-level init, TinyUSB task loop, UAC/CDC descriptors. |
| CLI thread | Command queue, async output queue, periodic link/audio status output. |
| PROP subsystem | Radio packet rings, app-facing PROP queues, session status and loss accounting. |
