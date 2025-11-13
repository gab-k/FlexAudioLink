cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Compiler options
set(STM32_MCU_FLAGS  "-mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard " )

# Linker options
# For whatever reason STM32CubeMX generates two different linker files depending on IDE selection
# This line below was changed to the one generated when generating for the STM32CubeIDE
set(STM32_LINKER_SCRIPT Appli/STM32H7S3L8HX_ROMxspi2.ld)
set(STM32_LINKER_OPTION  )

# Include toolchain file
include("../gcc-arm-none-eabi.cmake")

# Set the project name
set(CMAKE_PROJECT_NAME STM32_SW_Appli)
project(${CMAKE_PROJECT_NAME})

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Generate map file for Appli
target_link_options(${CMAKE_PROJECT_NAME}
  PUBLIC
    LINKER:-Map=${CMAKE_PROJECT_NAME}.map
)

# Include mx-generated 
include("mx-generated.cmake")


# In order to use CMake for cross-compiling
set(CMAKE_CXX_COMPILER_FORCED "true") 

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE 
    # Add user defined symbols
)

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
    ${CMAKE_SOURCE_DIR}/Appli/tinyusb/src
    ${CMAKE_SOURCE_DIR}/Appli/tinyusb/hw
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user sources here
    ${CMAKE_SOURCE_DIR}/Appli/Core/Src/pll.c
    ${CMAKE_SOURCE_DIR}/Appli/Core/Src/audio.c
    ${CMAKE_SOURCE_DIR}/Appli/Core/Src/usb_descriptors.c
    ${CMAKE_SOURCE_DIR}/Appli/tinyusb/src/tusb.c
    ${CMAKE_SOURCE_DIR}/Appli/tinyusb/src/device/usbd.c
    ${CMAKE_SOURCE_DIR}/Appli/tinyusb/src/device/usbd_control.c
    ${CMAKE_SOURCE_DIR}/Appli/tinyusb/src/common/tusb_fifo.c
    ${CMAKE_SOURCE_DIR}/Appli/tinyusb/src/class/audio/audio_device.c
    ${CMAKE_SOURCE_DIR}/Appli/tinyusb/src/portable/synopsys/dwc2/dcd_dwc2.c
    ${CMAKE_SOURCE_DIR}/Appli/tinyusb/src/portable/synopsys/dwc2/dwc2_common.c
)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined libraries
)

# Validate that STM32CubeMX code is compatible with C standard
if((CMAKE_C_STANDARD EQUAL 90) OR (CMAKE_C_STANDARD EQUAL 99))
    message(ERROR "Generated code requires C11 or higher")
endif()
