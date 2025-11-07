# External Flash Loaders

This directory contains external loader (`.stldr`) files required for flashing and debugging the application on the target's external flash memory.

### Why This Folder Exists

The debugger configuration in this project (located in `.vscode/launch.json`) needs to know how to communicate with the external memory chip connected to the STM32 microcontroller. Instead of using an absolute path to the STM32CubeProgrammer installation directory (e.g., `C:/Program Files/STMicroelectronics/...`), which would only work on a specific machine, we store these loaders directly within the project.

This approach makes the project **portable and self-contained**. Anyone can clone this repository and be able to flash and debug the hardware without manually configuring system-specific paths.

### File Origin and Purpose

These `.stldr` files are provided by STMicroelectronics and are copied from a standard installation of the **STM32CubeProgrammer** software.

*   **Source Location:** They are typically found in the `bin/ExternalLoader/` directory of the STM32CubeProgrammer installation path.
*   **Function:** An external loader is a small piece of code that the main programmer tool (like STM32CubeProgrammer or OpenOCD) downloads into the microcontroller's internal RAM. This code contains the necessary drivers and algorithms to erase, write, and verify an external memory chip (in this case, a HyperRAM or Octo-SPI flash).

### Included Loader Files

The files in this directory are for the **MX25UW25645G** memory chip, used with the **NUCLEO-H7S3L8** development board. The different suffixes specify the communication interface or context used.

*   `MX25UW25645G_NUCLEO-H7S3L8.stldr`
    *   The standard loader for this memory/board combination.

*   `MX25UW25645G_NUCLEO-H7S3L8-XSPIM1.stldr`
    *   **XSPIM1** specifies that the loader will use the `XSPIM` (eXpanded Serial Peripheral Interface Manager) peripheral, port 1, to communicate with the flash memory.


The correct loader to use depends on the project's specific hardware and software configuration.