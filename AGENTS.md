Low-latency wireless audio bridge.

* **USB-to-I2S:** Wired fallback (TinyUSB <-> I2S).
* **Dongle:** TinyUSB <-> Proprietary_2.4GHz/BLE.
* **Headset:** Proprietary_2.4GHz/BLE <-> I2S.

Currently porting from a RW612 firmware to a nRF54LM20A firmware, essentially switching from wifi to proprietary 2.4 GHz for maximum time determinism.

## Repo Shape

* `nrf_firmware/` is the active port target. It is a Zephyr-based firmware for `nrf54lm20dk/nrf54lm20a/cpuapp`.
* `rw612_firmware/` is the older NXP/MCUXpresso + FreeRTOS implementation. It is still valuable as design/reference code, especially for the original Wi-Fi transport and latency work.
* `webui/` is a separate Svelte app plus a Python WebSocket-to-serial bridge for device configuration over USB CDC ACM.
* `hardware/` contains the KiCad project and a hardware spec centered on the nRF54LM20A + NAU88L21 + nPM1300 design.
* `python/` contains small standalone utilities for signal generation, latency/error experiments, and UDP/audio testing.
* `docs/` and `devel_notes/` contain design notes and bring-up references; they are not build inputs, but they do capture a lot of project intent.

## Current Reality

* The root `README.md` still describes the earlier RW612/Wi-Fi architecture in detail. Treat it as historical context, not as the source of truth for what is currently implemented in `nrf_firmware/`.
* The nRF port is currently closer to a proprietary-radio bring-up / integration scaffold than a finished end-to-end audio bridge.
* In `nrf_firmware/src/main.c`, `main()` is intentionally idle; subsystems self-start via `K_THREAD_DEFINE(...)`.
* Runtime state is managed by `nrf_firmware/src/mode.c` with:
  role = `dongle` or `headset`
  mode = `proprietary`, `ble`, or `usb`
* The current mode logic rejects `dongle + usb`. Applying a mode change currently switches:
  USB profile between CDC-only and UAC+CDC
  proprietary test mode on/off
* The proprietary link code is currently a thin wrapper over `radio_hw.*`; the implemented path today is primarily the test-mode TX/RX flow, stats collection, and role auto-selection from device UID.
* The nRF CLI already exposes useful control/status commands over USB CDC (`help`, `get`, `set role`, `set mode`, `status`, `scan`, `linktest`, `reset`, `i2s tone ...`).

## Important Entry Points

* `nrf_firmware/src/mode.c` coordinates role/mode changes and decides default role from `hwinfo_get_device_id()`.
* `nrf_firmware/src/proprietary/radio_hw.h` defines the current radio-test constants, including fixed frequency, 4 Mbit mode, payload size, interval, and the `TX_DEVICE_ID` used to pick the default dongle.
* `nrf_firmware/src/proprietary/test_mode.c` contains the current always-on radio test threads that drive TX/RX when proprietary mode is enabled.
* `nrf_firmware/src/usb/usb_device.c` owns TinyUSB device bring-up and profile switching between CDC and UAC+CDC.
* `nrf_firmware/src/audio_io/` contains I2S/codec-facing code for the NAU88L21 side of the bridge.
* `rw612_firmware/raw_audio.c`, `rw612_firmware/udp.c`, `rw612_firmware/audio.c`, and `rw612_firmware/wifi_app.c` are the clearest references for the previous end-to-end streaming architecture.

## Build And Third-Party Boundaries

* `nrf_firmware/CMakePresets.json` defines the Zephyr configure/build preset `nrf54lm20dk_debug` and builds into `nrf_firmware/build/`.
* `rw612_firmware/CMakePresets.json` defines `flash_debug` and `flash_release` for the legacy MCUX SDK project.
* `nrf_firmware/tinyusb/` is a git submodule, not ordinary project code. Its `.gitmodules` entry points at `https://github.com/gab-k/tinyusb.git` on branch `nrf54lm20`.
* Prefer changing project-owned integration code in `nrf_firmware/src/usb/` before editing the vendored TinyUSB submodule unless the bug is clearly inside TinyUSB itself.
* There are generated/build outputs checked into firmware subtrees (`nrf_firmware/build/`, `rw612_firmware/flash_debug/`). Do not assume the repo is clean.

## Web UI Context

* `webui/app/` is a Svelte 5 + Vite app.
* The UI talks plain text line protocol over either:
  WebSerial in Chromium browsers
  `webui/websocket_serial_bridge.py` for Firefox/Safari fallback
* `webui/spec.md` is intentionally more forward-looking than the firmware. It describes the intended product/UI model, not necessarily what the nRF firmware already supports.

## Hardware Context

* The current hardware direction is a custom nRF54LM20A board with:
  NAU88L21 audio codec
  nPM1300 PMIC
  USB-C
  battery-backed operation
* `hardware/hw_spec.md` is the fastest way to recover rail assignments, codec interface assumptions, and schematic/layout reminders during board-facing work.

## Practical Guidance

* When working on "current product" behavior, start in `nrf_firmware/`.
* When trying to recover architecture, packet formats, or latency rationale, mine `rw612_firmware/` and `docs/low_latency_optimization.md`.
* If a request mentions Wi-Fi/raw L2/UDP transport, that is legacy RW612 context unless the user explicitly wants it ported.
* If a request mentions proprietary 2.4 GHz, BLE, USB Audio, USB CDC, or role auto-selection by UID, that maps to the nRF port.
