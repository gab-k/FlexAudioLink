cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(flexlink_radio_test VERSION 0.1.0 LANGUAGES C)

set(TINYUSB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tinyusb)

if(DEFINED ZEPHYR_HAL_NORDIC_MODULE_DIR)
    set(HAL_NORDIC_DIR ${ZEPHYR_HAL_NORDIC_MODULE_DIR})
else()
    set(HAL_NORDIC_DIR ${ZEPHYR_BASE}/../modules/hal/nordic)
endif()

target_sources(app PRIVATE
    src/main.c
    src/app_control.c
    src/cli.c
    src/audio/audio_path_common.c
    src/audio/audio_path_wired.c
    src/audio/audio_path_wireless_dongle.c
    src/audio/audio_path_wireless_headset.c
    src/audio/i2s.c
    src/audio/nau88l21.c
    src/prop_fsk/radio_core.c
    src/prop_fsk/session.c
    src/prop_fsk/test_mode.c
    src/usb/usb_device.c
    src/usb/usb_audio.c
    src/usb/usb_cdc.c
    src/usb/usb_descriptors.c
    ${TINYUSB_DIR}/src/tusb.c
    ${TINYUSB_DIR}/src/device/usbd.c
    ${TINYUSB_DIR}/src/device/usbd_control.c
    ${TINYUSB_DIR}/src/common/tusb_fifo.c
    ${TINYUSB_DIR}/src/class/cdc/cdc_device.c
    ${TINYUSB_DIR}/src/class/audio/audio_device.c
    ${TINYUSB_DIR}/src/portable/synopsys/dwc2/dcd_dwc2.c
    ${TINYUSB_DIR}/src/portable/synopsys/dwc2/dwc2_common.c
)

target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/usb
    ${ZEPHYR_BASE}/modules/hal_nordic/nrfx
    ${TINYUSB_DIR}/src
    ${TINYUSB_DIR}/hw
    ${TINYUSB_DIR}/hw/bsp
    ${TINYUSB_DIR}/hw/bsp/nrf
    ${TINYUSB_DIR}/hw/bsp/nrf/boards/nrf54lm20dk
    ${HAL_NORDIC_DIR}/nrfx
    ${HAL_NORDIC_DIR}/nrfx/lib
    ${HAL_NORDIC_DIR}/nrfx/mdk
    ${HAL_NORDIC_DIR}/nrfx/bsp/stable/mdk
    ${HAL_NORDIC_DIR}/nrfx/hal
    ${HAL_NORDIC_DIR}/nrfx/drivers/include
)

target_compile_definitions(app PRIVATE
    NRF54LM20A_ENGA_XXAA
)
