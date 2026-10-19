# PicoMultiLoader for the PicoCalc with a Raspberry Pi Pico (RP2040) or Pico 2 W (RP2350).
#
#   cmake -B build-rp2040 -G Ninja -DPICO_BOARD=pico     # -> BOOT2040.UF2 (no Wi-Fi)
#   cmake -B build-rp2350 -G Ninja -DPICO_BOARD=pico2_w  # -> BOOT2350.UF2 (Wi-Fi web file manager)
#   cmake --build build-rp2350
#
# The output is a RAM-only (no_flash) image meant to be started by the PicoCalc UF2 Loader's
# stage3 from the root of the SD card.
cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Everything lives in RAM (and on the Pico 2 W the radio firmware alone takes 225 KB): optimise for size.
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE MinSizeRel CACHE STRING "Build type")
endif()

if (NOT PICO_BOARD)
    set(PICO_BOARD pico2_w CACHE STRING "Board type")
endif()

# Only affects the .dis listings; some picotool builds crash in coprodis on Windows.
set(PICO_NO_COPRO_DIS 1)

include(pico_sdk_import.cmake)

project(picomultiloader C CXX ASM)

# Use an existing picotool (e.g. -DPICOTOOL_EXE=C:/path/picotool.exe) instead of the one the SDK builds
# from source, which can crash when the host compiler is very new.
set(PICOTOOL_EXE "" CACHE FILEPATH "picotool executable to use (empty: let the SDK find or build one)")
if (PICOTOOL_EXE)
    add_executable(picotool IMPORTED GLOBAL)
    set_target_properties(picotool PROPERTIES IMPORTED_LOCATION "${PICOTOOL_EXE}")
endif()

pico_sdk_init()

if (PICO_PLATFORM MATCHES "rp2040")
    set(LOADER_NAME BOOT2040)
else()
    set(LOADER_NAME BOOT2350)
endif()

set(LOADER_WIFI 0)
if (PICO_CYW43_SUPPORTED AND NOT PICO_PLATFORM MATCHES "rp2040")
    set(LOADER_WIFI 1)
endif()

add_executable(${LOADER_NAME}
    src/main.c
    src/config.c
    src/keyboard.c
    src/lcd.c
    src/sdcard.c
    src/uf2_image.c
    src/ui.c
    src/usb_descriptors.c
    src/usb_drive.c
    lib/fatfs/ff.c
    lib/fatfs/ffunicode.c
)

target_include_directories(${LOADER_NAME} PRIVATE
    src
    lib/fatfs
    ${CMAKE_CURRENT_LIST_DIR}/../common
)

target_compile_definitions(${LOADER_NAME} PRIVATE
    PICOMULTILOADER_WIFI=${LOADER_WIFI}
    PICO_STDIO_UART=1
    PICO_PRINTF_SUPPORT_FLOAT=0
    PICO_PRINTF_SUPPORT_EXPONENTIAL=0
)

target_compile_options(${LOADER_NAME} PRIVATE -Wall -Wno-unused-function)

target_link_libraries(${LOADER_NAME}
    pico_stdlib
    pico_unique_id
    hardware_spi
    hardware_i2c
    hardware_dma
    hardware_flash
    hardware_watchdog
    tinyusb_device
    tinyusb_board
    boot_uf2_headers
)

if (LOADER_WIFI)
    # The browser page is compiled in as a C string.
    set(INDEX_HTML ${CMAKE_CURRENT_LIST_DIR}/../common/web/index.html)
    set(INDEX_H ${CMAKE_CURRENT_BINARY_DIR}/generated/index_html.h)
    add_custom_command(
        OUTPUT ${INDEX_H}
        COMMAND ${CMAKE_COMMAND} -DIN=${INDEX_HTML} -DOUT=${INDEX_H} -P ${CMAKE_CURRENT_LIST_DIR}/embed.cmake
        DEPENDS ${INDEX_HTML} ${CMAKE_CURRENT_LIST_DIR}/embed.cmake
    )
    target_sources(${LOADER_NAME} PRIVATE src/webserver.c src/dhcpserver.c ${INDEX_H})
    target_include_directories(${LOADER_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_compile_definitions(${LOADER_NAME} PRIVATE CYW43_HOST_NAME="picocalc")
    target_link_libraries(${LOADER_NAME}
        pico_cyw43_arch_lwip_poll
        pico_lwip_mdns
        pico_rand
    )
endif()

pico_set_binary_type(${LOADER_NAME} no_flash)
pico_enable_stdio_uart(${LOADER_NAME} 1)
pico_enable_stdio_usb(${LOADER_NAME} 0)
target_link_options(${LOADER_NAME} PRIVATE -Wl,--print-memory-usage)
pico_add_extra_outputs(${LOADER_NAME})
