# nrf_firmware

Zephyr firmware for `nrf54lm20dk/nrf54lm20a/cpuapp`.

## New Machine Setup

1. Install host packages on Arch / EndeavourOS:

```bash
sudo pacman -S cmake ninja dtc gperf
yay -S python-west nrfutil
```

2. Install the nRF Connect SDK source tree into `~/ncs`:

```bash
mkdir -p ~/ncs
cd ~/ncs
west init -m https://github.com/nrfconnect/sdk-nrf --mr v3.2.4 v3.2.4
cd ~/ncs/v3.2.4
west update
west zephyr-export
```

3. Install the matching Nordic-managed toolchain with `nrfutil`:

```bash
nrfutil install sdk-manager          # once
nrfutil sdk-manager install v3.2.4   # once per SDK version
```

4. Clone this repo and fetch the TinyUSB submodule:

```bash
cd ~
git clone https://github.com/gab-k/FlexAudioLink.git
cd ~/FlexAudioLink
git submodule update --init --recursive nrf_firmware/tinyusb
```

5. Export `ZEPHYR_BASE`:

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

## Build

Launch a shell with the Nordic-managed toolchain first:

```bash
nrfutil sdk-manager toolchain launch --ncs-version v3.2.4 --shell
```

Then build:

Standard build:

```bash
cd ~/FlexAudioLink/nrf_firmware
west build -b nrf54lm20dk/nrf54lm20a/cpuapp . --build-dir build
```

Pristine build:

```bash
cd ~/FlexAudioLink/nrf_firmware
west build -p always -b nrf54lm20dk/nrf54lm20a/cpuapp . --build-dir build
```


## VS Code Tasks

[`tasks.json`](/home/gab/FlexAudioLink/.vscode/tasks.json) provides:

- `west: build`
- `west: pristine build`

The VS Code tasks launch `west` through `nrfutil sdk-manager toolchain launch`, so they do not need hard-coded SDK bundle paths.
