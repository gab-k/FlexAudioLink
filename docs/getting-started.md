# Getting Started

## Prerequisites

Install host packages on Arch / EndeavourOS:

```bash
sudo pacman -S cmake ninja dtc gperf
yay -S python-west nrfutil
```

## Install the nRF Connect SDK

1. Clone the SDK source tree:

```bash
mkdir -p ~/ncs
cd ~/ncs
west init -m https://github.com/nrfconnect/sdk-nrf --mr v3.2.4 v3.2.4
cd ~/ncs/v3.2.4
west update
west zephyr-export
```

2. Install the matching Nordic-managed toolchain:

```bash
nrfutil install sdk-manager          # once
nrfutil sdk-manager install v3.2.4   # once per SDK version
```

3. Export `ZEPHYR_BASE`:

For `bash` / POSIX shells:

```bash
export ZEPHYR_BASE=~/ncs/v3.2.4/zephyr
```

Persistent in `fish`:

```fish
set -Ux ZEPHYR_BASE $HOME/ncs/v3.2.4/zephyr
```

Persistent in `bash`:

```bash
# ~/.bashrc
export ZEPHYR_BASE="$HOME/ncs/v3.2.4/zephyr"
```

## Clone and Build

Clone the repo and fetch the TinyUSB submodule:

```bash
cd ~
git clone https://github.com/gab-k/FlexAudioLink.git
cd ~/FlexAudioLink
git submodule update --init --recursive firmware/tinyusb
```

Launch a shell with the Nordic toolchain:

```bash
nrfutil sdk-manager toolchain launch --ncs-version v3.2.4 --shell
```

Build:

```bash
cd ~/FlexAudioLink/firmware
west build -b nrf54lm20dk/nrf54lm20a/cpuapp . --build-dir build
```

Pristine build:

```bash
cd ~/FlexAudioLink/firmware
west build -p always -b nrf54lm20dk/nrf54lm20a/cpuapp . --build-dir build
```

## VS Code Tasks

`tasks.json` provides:

- `west: build`
- `west: pristine build`

The VS Code tasks launch `west` through `nrfutil sdk-manager toolchain launch`, so they do not need hard-coded SDK bundle paths.

## Flash and Debug

The nrf54lm20dk eval board has an onboard Segger J-Link debugger. Flashing and debugging are done through VS Code using the Cortex-Debug extension with J-Link.

### Prerequisites

- Segger J-Link software installed on the host
- [Cortex-Debug](https://marketplace.visualstudio.com/items?itemName=marus25.cortex-debug) VS Code extension

### Flashing

The `launch.json` configurations build and flash in one step. Select the configuration matching your board's serial number (replace the serial number in `launch.json` with your own) and start debugging:

- `Debug (1051811555)` — board 1
- `Debug (1051890941)` — board 2

Both configurations:

- Build the firmware first (`preLaunchTask: west: build`)
- Flash via J-Link SWD to the `NRF54LM20A_M33` core
- Break at `main` on entry
- Enable RTT output

To flash without debugging:

```bash
nrfutil device program --firmware build/firmware/zephyr/zephyr.elf --traits nrf54lm20dk
```

Or via J-Link CLI:

```bash
JLinkExe -device NRF54LM20A_M33 -if SWD -speed 4000 -AutoErase
J-Link> loadfile build/firmware/zephyr/zephyr.elf
J-Link> r
J-Link> g
J-Link> exit
```

## Serial Console

Firmware log output (`printk` / `LOG_*`) goes to UART. Use any serial terminal at 115200 baud, or:

```bash
python python/q_term.py
```
