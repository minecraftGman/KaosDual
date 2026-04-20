cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)

project(kaos_pico C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(kaos_pico
    main.c
    usb_descriptors.c
    skylander_slots.c
    rijndael.c
    SkylanderCrypt.c
)

# Tell TinyUSB where to find tusb_config.h
target_include_directories(kaos_pico PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Pull in TinyUSB via Pico SDK
target_link_libraries(kaos_pico
    pico_stdlib
    pico_multicore
    hardware_uart
    tinyusb_device
    tinyusb_board
)

# UART1 on GPIO4/5, USB on the default USB pins
pico_enable_stdio_uart(kaos_pico 0)   # disable stdio on UART (we use it for protocol)
pico_enable_stdio_usb(kaos_pico 0)    # disable stdio on USB  (we use it for HID)

pico_add_extra_outputs(kaos_pico)
