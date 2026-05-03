# FlexAudioLink

Low-latency wireless audio bridge.

## Motivation and Use-cases

FlexAudioLink originated as a low-latency wireless headset transceiver for gaming. The hardware and firmware architecture is general enough to cover a wider range of audio bridging scenarios:

- Wireless audio relay (e.g. room-to-room)
- BLE audio bridging (phone/laptop as central to a FlexAudioLink node)
- Arbitrary Source/Sink combinations — each audio endpoint can be I2S (analog codec) or USB Device, supporting USB-to-analog, USB-to-USB, analog-to-analog, and BLE bridges

Most of these are not yet implemented; see [Project Status](#project-status).

## Features

- Audio bridge endpoints: USB Audio Class device, I2S/analog codec, BLE
- Low-latency wireless link between nodes
- USB CDC CLI for device configuration
- Web UI (Svelte) with WebSerial and WebSocket-to-serial bridge
- Single hardware design: nRF54LM20A + NAU88L21 + nPM1300

## Architecture

Two identical FlexAudioLink nodes form a wireless audio bridge. Each node runs the same firmware and carries the same hardware (nRF54LM20A, NAU88L21, nPM1300). Configuration determines which interfaces are active at each endpoint.

![FlexAudioLink wireless headset use case](docs/dongle_headset_usecase.drawio.svg)

In the dongle configuration, USB is the active audio interface. In the headset configuration, I2S to the codec drives the speakers and captures the microphone. Audio flows bidirectionally over the wireless link between the two nodes.

## Project Status

**Implemented:**
- USB device mode (UAC + CDC)
- Boot profile persistence
- USB CDC CLI
- PFSK radio test mode
- Audio I2S / codec integration
- Wireless audio streaming (USB ↔ I2S)

**In progress:**
- Schematic

**Planned:**
- PCB layout
- Arbitrary Source/Sink combinations
- Analog endpoint multiplexing
- Packet Loss Concealment
- BLE audio transport
- Web UI

**Under evaluation:**
- Adaptive Frequency Hopping
- Clear Channel Assessment


## Documentation

More detailed and technical documentation lives [here](https://gab-k.github.io/FlexAudioLink/). It most importantly includes:

- [**Getting Started**](https://gab-k.github.io/FlexAudioLink/getting-started/) — toolchain setup, build, and flash instructions
- [**Firmware**](https://gab-k.github.io/FlexAudioLink/firmware/) — architecture, PFSK protocol
- [**Hardware**](https://gab-k.github.io/FlexAudioLink/hardware/) — schematic, PCB, rationale for chosen components
- [**Web UI**](https://gab-k.github.io/FlexAudioLink/webui/) — setup and configuration
- [**CLI Reference**](https://gab-k.github.io/FlexAudioLink/cli/) — available CLI commands

Note: the documentation is a work in progress and nowhere near complete.

## Repository Layout

- `firmware/`: active Zephyr-based firmware for `nrf54lm20dk/nrf54lm20a/cpuapp`
- `webui/`: Svelte configuration UI plus Python WebSocket-to-serial bridge
- `hardware/`: KiCad project and hardware notes
- `python/`: standalone utilities for signal generation and latency/error experiments
- `devel_notes/`: bring-up and development references
