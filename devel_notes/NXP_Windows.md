# Windows Notes

## 1. Prerequisites

Install the following tools:

- **MCUXpresso for VS Code** toolset from [nxp.com](https://www.nxp.com/design/design-center/software/development-software/mcuxpresso-software-and-tools-/mcuxpresso-for-visual-studio-code:MCUXPRESSO-VSC): provides the ARM toolchain, Python venv, and mcux-fixelf post-processor.
- **west** meta-tool: installed inside the MCUXpresso Python venv, or via `pip install west`.
- **Ninja**: included with MCUXpresso tools, or install via [ninja-build.org](https://ninja-build.org/).
- **CMake** 3.27+: [cmake.org](https://cmake.org/download/).
- **J-Link Software**: [segger.com/downloads/jlink](https://www.segger.com/downloads/jlink/).

## 2. NXP Workspace Initialization

```bat
cd C:\NXP
west init -m https://github.com/nxp-mcuxpresso/mcuxsdk-manifests.git --mr v25.12.00 mcuxsdk
cd mcuxsdk
west update
```

## 3. CMake Environment Variables

`mcux_include.json` reads `SdkRootDirPath` and `ARMGCC_DIR` from the process environment via `$penv{}`. Set them as user environment variables (Win + R → `sysdm.cpl` → Advanced → Environment Variables):

| Variable | Value |
|---|---|
| `SdkRootDirPath` | `C:\NXP\mcuxsdk` (west workspace root, parent of `mcuxsdk\`) |
| `ARMGCC_DIR` | `C:\Users\<you>\.mcuxpressotools\arm-gnu-toolchain-14.2.rel1-mingw-w64-x86_64-arm-none-eabi` |

Restart VS Code after setting them.

## 4. VS Code Extensions

Same extensions as EOS (see `NXP_EOS.md` §3), all available in the VS Code Marketplace.

## 5. Building

Open `firmware/` as the workspace folder. Select the `flash_debug` or `flash_release` CMake preset from the CMake Tools status bar, then build.
