cmake_minimum_required(VERSION 3.13)

# PGA2350 module (RP2350B, 48 GPIO, 16 MiB QSPI flash): in-SDK board, bare
# RP2350B, no USB device.
set(PICO_BOARD pimoroni_pga2350 CACHE STRING "Board type")
set(PICO_PLATFORM rp2350 CACHE STRING "Pico platform")
set(PICO_FLASH_SIZE_BYTES 16777216 CACHE STRING "Flash size in bytes (16 MiB)")

include(pico_sdk_import.cmake)

project(nescd C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# src/hcd_rp2040_epx.c replaces the SDK's host controller driver: bulk OUT needs
# EPX to escape the async endpoints' 64 B/frame ceiling.
get_target_property(_tuh_srcs tinyusb_host_base INTERFACE_SOURCES)
list(FILTER _tuh_srcs EXCLUDE REGEX "hcd_rp2040\\.c$")
set_property(TARGET tinyusb_host_base PROPERTY INTERFACE_SOURCES ${_tuh_srcs})

add_executable(nescd
    src/main.c
    src/ata.c
    src/atapi.c
    src/audio.c
    src/bgm.c
    src/catalog.c
    src/cdcore.c
    src/diag.c
    src/disc.c
    src/disctask.c
    src/edn8.c
    src/eject.c
    src/hcd_rp2040_epx.c
    src/jingle.c
    src/knob.c
    src/mailbox.c
    src/n8push.c
    src/psram.c
    src/tone.c
    src/track.c
    src/usb_link.c
)
pico_generate_pio_header(nescd ${CMAKE_CURRENT_LIST_DIR}/src/ata.pio)
pico_generate_pio_header(nescd ${CMAKE_CURRENT_LIST_DIR}/src/audio_i2s.pio)

target_include_directories(nescd PRIVATE
    src
    ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/rp2040   # rp2040_usb.h
)
target_link_libraries(nescd
    pico_stdlib
    hardware_gpio
    hardware_adc
    hardware_pio
    hardware_dma
    hardware_sync
    hardware_clocks
    hardware_xip_cache
    pico_multicore
    tinyusb_host
)

# UART stdio (GP0 TX / GP1 RX), read over the debugprobe bridge. The USB
# peripheral is the host port to the EverDrive, so USB stdio stays off.
pico_enable_stdio_usb(nescd 0)
pico_enable_stdio_uart(nescd 1)

# core1 nests atapi_packet -> request_sense -> atapi_packet; the SDK default
# stack is 0x800.
target_compile_definitions(nescd PRIVATE PICO_CORE1_STACK_SIZE=4096)

pico_add_extra_outputs(nescd)
