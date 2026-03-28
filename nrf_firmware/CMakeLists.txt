cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(flexlink_radio_test VERSION 0.1.0 LANGUAGES C)

target_sources(app PRIVATE
    src/main.c
    src/radio.c
    src/tx.c
    src/rx.c
)

target_include_directories(app PRIVATE
    ${ZEPHYR_BASE}/modules/hal/nordic/nrfx/hal
)