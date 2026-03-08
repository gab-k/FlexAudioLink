# Copyright 2025 NXP
#
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.22.0)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(${SdkRootDirPath}/cmake/extension/mcux.cmake)

file(RELATIVE_PATH BOARD_REL_PATH "${SdkRootDirPath}" "${CMAKE_CURRENT_SOURCE_DIR}")

project(FlexAudioLink LANGUAGES C CXX ASM PROJECT_BOARD_PORT_PATH ${BOARD_REL_PATH})

include(${SdkRootDirPath}/CMakeLists.txt)


mcux_add_source(
    SOURCES 
            # HW config related
            peripherals.c
            peripherals.h
            clock_config.c
            clock_config.h
            hardware_init.c
            pin_mux.c
            pin_mux.h
            board.c
            board.h
            
            # Application related
            main.c
            audio.c
            cli.c
            cli.h
            wifi_app.c
            wifi_app.h
            udp.c
            udp.h
            mode.c
            mode.h
            util.c
            util.h
            log.c
            log.h

            # LwIP related
            lwipopts.h
            lwippools.h

            # FreeRTOS related
            FreeRTOSConfig.h
            FreeRTOSConfigBoard.h
            
            # TinyUSB descriptor and configuration files
            usb_descriptors.c
            tusb_config.h

            # TinyUSB internal files
            tinyusb/src/tusb.c
            tinyusb/src/device/usbd.c
            tinyusb/src/device/usbd_control.c
            tinyusb/src/common/tusb_fifo.c
            tinyusb/src/class/audio/audio_device.c
            tinyusb/src/class/cdc/cdc_device.c
            tinyusb/src/portable/ehci/ehci.c
            tinyusb/src/portable/chipidea/ci_hs/dcd_ci_hs.c
)

mcux_add_include(
    INCLUDES    .
                ./tinyusb/src
                ./tinyusb/hw
)

# --------------------------------------------------------------------------
# GLOBAL HEADER INJECTION & LOG REDIRECTION
# --------------------------------------------------------------------------
# Force the compiler to include 'log.h' at the top of every compilation unit 
# (both .c files and SDK sources).
#
# This allows 'log.h' to:
# 1. Intercept the standard SDK 'fsl_debug_console.h'
# 2. Undefine the SDK's blocking 'PRINTF' macro
# 3. Redefine 'PRINTF' to point to 'log_print' function
target_compile_options(FlexAudioLink PRIVATE 
    "-include${CMAKE_CURRENT_SOURCE_DIR}/log.h"
    #"-DCONFIG_11AX=0" 
    #"-DCONFIG_11AC=0"
)

mcux_convert_binary(BINARY ${APPLICATION_BINARY_DIR}/${MCUX_SDK_PROJECT_NAME}.bin)


mcux_add_macro(
    CC "PRINTF_ADVANCED_ENABLE=1"
    TARGETS flash_debug flash_release
)

mcux_add_linker_symbol(
    SYMBOLS "__stack_size__=0x2000 __heap_size__=0x0400"
)

# TODO: Perhaps this can be done by Kconfig?
# This removes the default RAM linker script added by SDK
mcux_remove_armgcc_linker_script(
    BASE_PATH ${SdkRootDirPath}
    TARGETS flash_debug flash_release
    LINKER ${SdkRootDirPath}/devices/Wireless/RW/${CONFIG_MCUX_TOOLCHAIN_LINKER_DEVICE_PREFIX}/gcc/${CONFIG_MCUX_TOOLCHAIN_LINKER_DEVICE_PREFIX}_flash.ld
)

# Assign external FLASH linker script ONLY to the Flash targets
mcux_add_armgcc_linker_script(
    BASE_PATH ${SdkRootDirPath}
    TARGETS flash_debug flash_release
    LINKER ${CMAKE_CURRENT_SOURCE_DIR}/RW61x_flash.ld
)

