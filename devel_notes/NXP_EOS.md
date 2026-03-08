# EndeavourOS (Arch) Notes

## 1. Environment Setup & Dependencies

Standard Arch/AUR packages required for the NXP MCUXpresso SDK and debugging:

```bash
# Install toolchain and debugger
sudo pacman -S arm-none-eabi-gcc arm-none-eabi-gdb

# Install J-Link drivers & Python deps (via AUR)
yay -S jlink-software-and-documentation python-jsonschema

# Install Zephyr's 'west' meta-tool (via AUR)
yay -S python-west
```

## 2. NXP Workspace Initialization

Get NXP mcuxpresso SDK using west:

```bash
cd ~
# Initialize workspace with specific release
west init -m https://github.com/nxp-mcuxpresso/mcuxsdk-manifests.git --mr v25.12.00 nxp-workspace
cd nxp-workspace/

# Fetch all repositories
west update
```

## 3. VSCodium / VS Code Extensions


- **clangd** (`llvm-vs-code-extensions.vscode-clangd`): Intellisense and `.clang-format`. Requires `compile_commands.json` exported by CMake.
- **CMake Tools** (`ms-vscode.cmake-tools`): Configures and builds the project.
- **Cortex-Debug** (`marus25.cortex-debug`): Core GDB debugging UI for J-Link/ARM.
- **debug-tracker-vscode**: Dependency for advanced debug views.
- **MemoryView**: Live memory hex inspector during debugging.
- **Peripheral Viewer**: Shows MCU hardware registers (requires `.svd` file in `launch.json`).
- **RTOS Views**: Thread-awareness for FreeRTOS (view task states, stacks, etc.).


## 4. CMake Environment Variables

`mcux_include.json` uses `$penv{}` to read `SdkRootDirPath` and `ARMGCC_DIR` from the process environment. VS Code launched from the desktop doesn't inherit `.bashrc`, so set these via systemd's user environment — create `~/.config/environment.d/nxp.conf`:

```ini
SdkRootDirPath=/home/gab/nxp-workspace
ARMGCC_DIR=/usr/
```

**Log out and back in** for the desktop session to pick them up. Verify with:

```bash
printenv SdkRootDirPath
```

> `SdkRootDirPath` must point to the west workspace root (parent of `mcuxsdk/`). CMake appends `/mcuxsdk` internally.

## 5. Hardware & Debugging (udev rules)

If Cortex-Debug throws `Connecting to J-Link failed`, or `lsusb` shows the device but you can't connect, it's a Linux USB permission issue.

Force the OS to reload the J-Link udev security rules:

```bash
sudo udevadm control --reload-rules
sudo udevadm trigger
```

> Unplug and re-plug the USB cable afterward.
