# FlexAudioLink

Low-latency wireless audio bridge.

## Scope

FlexAudioLink is intended to bridge audio and audio transport endpoints in the following combinations:

- USB Audio to analog audio
- USB Audio to USB Audio
- analog audio to analog audio
- BLE central device to analog audio
- BLE central device to USB Audio

The bridge may be configured for:

- duplex operation
- simplex operation

In this context:

- *analog audio* refers to the codec / analog I/O side of the system
- *USB Audio* refers to USB Audio Class device operation
- *BLE central device* refers to a host such as a phone or laptop acting as the central

All FlexAudioLink nodes use the same hardware and firmware base and are configured at runtime for the required operating mode.

## Repository Layout

| Directory | Description |
|-----------|-------------|
| `firmware/` | Active Zephyr-based firmware for `nrf54lm20dk/nrf54lm20a/cpuapp` |
| `webui/` | Svelte configuration UI plus Python WebSocket-to-serial bridge |
| `hardware/` | KiCad project and hardware notes for the nRF54LM20A + NAU88L21 + nPM1300 design |
| `python/` | Standalone utilities for signal generation and latency/error experiments |
| `devel_notes/` | Bring-up and development references |

## Roadmap

=== "Reached"

    - USB device bring-up with profile-selected descriptors (UAC+CDC or CDC-only)
    - persisted boot profile selection between `usb`, `pfsk_dongle`, and `pfsk_headset`
    - USB CDC CLI
    - USB audio mode
    - PFSK radio test mode, including TX/RX traffic and status counters
    - initial audio I2S / codec integration scaffolding

=== "In progress"

    - end-to-end wireless audio streaming
    - BLE audio transport
    - full realization of the configuration surface described by the web UI specification