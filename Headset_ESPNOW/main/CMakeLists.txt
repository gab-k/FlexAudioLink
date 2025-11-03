set(SRC_FILES "app_main.c" "dongle.c" "headset.c")

idf_component_register(SRCS "${SRC_FILES}"
                    PRIV_REQUIRES nvs_flash esp_event esp_netif esp_wifi esp_driver_i2s esp_driver_gpio esp_ringbuf
                    INCLUDE_DIRS ".")
                    
                    


# ON = Build for Headset; OFF = Build for Dongle
option(BUILD_FOR_HEADSET "Build the firmware for the Headset or Dongle" OFF)

if(BUILD_FOR_HEADSET)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "BUILD_HEADSET")
    message(STATUS "Building for Headset")
    message(STATUS "Building for Headset")
    message(STATUS "Building for Headset")
    message(STATUS "Building for Headset")
    message(STATUS "Building for Headset")
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE "BUILD_DONGLE")
    message(STATUS "Building for Dongle")
    message(STATUS "Building for Dongle")
    message(STATUS "Building for Dongle")
    message(STATUS "Building for Dongle")
    message(STATUS "Building for Dongle")
endif()

