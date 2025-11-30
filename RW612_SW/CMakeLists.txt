
# Copyright 2025 NXP
#
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.22.0)

include(${SdkRootDirPath}/cmake/extension/mcux.cmake)

file(RELATIVE_PATH BOARD_REL_PATH "${SdkRootDirPath}" "${CMAKE_CURRENT_SOURCE_DIR}")

project(wireless_headset LANGUAGES C CXX ASM PROJECT_BOARD_PORT_PATH ${BOARD_REL_PATH})

include(${SdkRootDirPath}/CMakeLists.txt)


mcux_add_source(
    SOURCES main.c
            #board.c
            #clock_config.c
            #pin_mux.c
            #peripherals.c
)

mcux_add_include(
    INCLUDES .
)
#include(${SdkRootDirPath}/${board_root}/${board}/freertos_examples/freertos_hello/reconfig.cmake OPTIONAL)

mcux_convert_binary(BINARY ${APPLICATION_BINARY_DIR}/${MCUX_SDK_PROJECT_NAME}.bin)

