# nRF Connect SDK — EndeavourOS (Arch) Notes

## 1. Prerequisites

```bash
# Toolchain, build tools, device tree compiler
sudo pacman -S arm-none-eabi-gcc arm-none-eabi-gdb cmake ninja dtc gperf

# west meta-tool (AUR)
yay -S python-west

# J-Link drivers (AUR)
yay -S jlink-software-and-documentation
```

## 2. NCS Installation (via nRF Connect VS Code Extension)

The recommended approach is to use the **nRF Connect for VS Code** extension, which installs everything to `~/ncs/`:

- `~/ncs/v3.2.4/` — NCS source code
- `~/ncs/toolchains/` — Zephyr SDK 0.17.0 + ARM toolchain
- `~/ncs/downloads/` — Download cache

Install the extension, then use the **nRF Connect: Install nRF Connect SDK** command to download NCS v3.2.4.

> Use **v3.2.4** or later — the `nrf54lm20dk` board target was not present in earlier releases.

## 3. Environment Variables

`west build` requires `ZEPHYR_BASE` when the app lives outside the NCS workspace tree. Set it via systemd's user environment — create `~/.config/environment.d/nrf.conf`:

```ini
ZEPHYR_BASE=/home/gab/ncs/v3.2.4/zephyr
```

**Log out and back in** for the desktop session to pick it up. Verify with:

```bash
printenv ZEPHYR_BASE
```

## 4. VS Code Extensions

- **nRF Connect for VS Code** (`nordic-semiconductor.nrf-connect`): Installs and manages NCS, toolchains, and provides build/flash UI.
- **clangd** (`llvm-vs-code-extensions.vscode-clangd`): Intellisense and `.clang-format`. Requires `compile_commands.json` exported by the build.
- **CMake Tools** (`ms-vscode.cmake-tools`): Configures and builds the project.
- **Cortex-Debug** (`marus25.cortex-debug`): GDB debugging UI for J-Link/ARM.

Zephyr exports `compile_commands.json` into the build directory. Point clangd at it by adding a `.clangd` file at the repo root:

```yaml
CompileFlags:
  CompilationDatabase: nrf_firmware/build
```

## 5. Building

Apps live in `FlexAudioLink/nrf_firmware/` — outside the NCS workspace, which is the standard Zephyr pattern.

Using the VS Code extension (recommended):
1. Open the nRF Connect panel
2. Select the `nrf_firmware` application
3. Choose board target: `nrf54lm20dk/nrf54lm20a/cpuapp`
4. Click Build

Or via command line using CMake:

```bash
cd ~/FlexAudioLink/nrf_firmware

# Configure
cmake -B build -GNinja -DBOARD=nrf54lm20dk/nrf54lm20a/cpuapp .

# Build
ninja -C build

# Clean rebuild
rm -rf build && cmake -B build -GNinja -DBOARD=nrf54lm20dk/nrf54lm20a/cpuapp . && ninja -C build
```

## 6. Flashing & Debugging (J-Link)

The nRF54LM20-DK has an onboard SEGGER J-Link OB debugger. No external programmer needed.

Via VS Code extension:
1. Connect the board via USB
2. Use the nRF Connect panel to flash

Or via command line:
```bash
# Flash using nrfutil
nrfutil device program --firmware build/zephyr/zephyr.hex --traits nrf54lm20dk

# Or using J-Link directly
JLinkExe -device nRF54LM20A_xxAA -if SWD -speed 4000 -autoconnect 1 \
    -CommandFile <<EOF
loadfile build/zephyr/zephyr.hex
r
g
exit
EOF
```

If `nrfutil` is not installed:
```bash
pip install nrfutil
nrfutil install device
```

## 7. RTT Console

RTT console output (replaces UART for logging on nRF54L series):

```bash
JLinkRTTClient
```

Or via **Cortex-Debug** extension: set `"rttConfig": { "enabled": true }` in `launch.json`.
