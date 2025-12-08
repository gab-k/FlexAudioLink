
# Copyright 2025 NXP
#
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.22.0)

include(${SdkRootDirPath}/cmake/extension/mcux.cmake)

file(RELATIVE_PATH BOARD_REL_PATH "${SdkRootDirPath}" "${CMAKE_CURRENT_SOURCE_DIR}")

project(rw612_sw LANGUAGES C CXX ASM PROJECT_BOARD_PORT_PATH ${BOARD_REL_PATH})

include(${SdkRootDirPath}/CMakeLists.txt)


mcux_add_source(
    SOURCES 
            # Regular application files
            main.c
            audio.c
            peripherals.c
            clock_config.c
            
            # TinyUSB descriptor and configuration files
            usb_descriptors.c
            tusb_config.h

            # TinyUSB internal files
            tinyusb/src/tusb.c
            tinyusb/src/device/usbd.c
            tinyusb/src/device/usbd_control.c
            tinyusb/src/common/tusb_fifo.c
            tinyusb/src/class/audio/audio_device.c
            tinyusb/src/portable/ehci/ehci.c
            tinyusb/src/portable/chipidea/ci_hs/dcd_ci_hs.c
)

mcux_add_include(
    INCLUDES    .
                ./tinyusb/src
                ./tinyusb/hw
)

mcux_convert_binary(BINARY ${APPLICATION_BINARY_DIR}/${MCUX_SDK_PROJECT_NAME}.bin)

mcux_add_linker_symbol(
    SYMBOLS "__stack_size__=0x2000 __heap_size__=0x0400"
)

mcux_add_macro(
    CC "PRINTF_ADVANCED_ENABLE=1"
    TARGETS debug
)