# Firmware

The FlexAudioLink firmware is a Zephyr application targeting `nrf54lm20dk/nrf54lm20a/cpuapp`.

## Implementation Notes

- `firmware/src/main.c`: loads the persisted boot profile and starts that profile's audio path
- `firmware/src/app_control.c`: owns boot profile persistence and startup
- PFSK profiles currently enable the present radio test-mode path; they are not yet the final audio transport

## Build

For toolchain setup and build steps, see `firmware/README.md`.

## Key Source Files

| File | Purpose |
|------|---------|
| `firmware/src/main.c` | Loads the persisted boot profile and starts that profile's audio path |
| `firmware/src/app_control.c` | Owns boot profile persistence and startup |
| `firmware/src/cli.c` | USB CDC CLI interface |

## Profiles

The firmware supports three boot profiles:

- **`usb`** — USB audio class device mode
- **`pfsk_dongle`** — Proprietary 2.4 GHz FSK dongle mode
- **`pfsk_headset`** — Proprietary 2.4 GHz FSK headset mode

Profiles are persisted and loaded on boot. Use the [CLI reference](../cli.md) `set profile` command to change profiles. Profile change requires reboot!