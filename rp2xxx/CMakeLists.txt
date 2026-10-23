# PicoMultiLoader for the PicoCalc with a Raspberry Pi Pico (RP2040) or Pico 2 W (RP2350).
#
#   cmake -B build-rp2040 -G Ninja -DPICO_BOARD=pico     # -> PicoMultiLoader-rp2040.uf2 (no Wi-Fi)
#   cmake -B build-rp2350 -G Ninja -DPICO_BOARD=pico2_w  # -> PicoMultiLoader-rp2350.uf2 (Wi-Fi web file manager)
#   Each also builds the classic menu (ui_classic.c): PicoMultiLoader-rp2040-classic.uf2 / pml2350-classic.uf2.
#   cmake --build build-rp2350
#
# Flash layout:
#   RP2040:  0x10000000 boot2 (enters the loader) + apps | 0x101BF000 app record | 0x101C0000 loader
#            PicoMultiLoader-rp2040.uf2 is all of it (flash it once through BOOTSEL).
#   RP2350:  0x10000000 boot stub + partition table | 0x10002000 apps (to the end of flash), the
#            same layout as pelrun's uf2loader. PicoMultiLoader-rp2350.uf2 is the stub (BOOTSEL);
#            the loader itself, pml2350.uf2 (or pml2350-classic.uf2, see stub/stub.c), goes in the
#            root of the SD card and runs from RAM.
cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# The loader must stay small (it shares flash with the apps): optimise for size.
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

# Two loaders from the same drivers: the graphical one (ui.c: Home, Files, the control center...)
# and the classic file list of earlier versions (ui_classic.c). On the RP2350 the boot stub loads
# one or the other (the "ui" setting, see stub/stub.c); on the RP2040, flash the one you want.
if (PICO_PLATFORM MATCHES "rp2040")
    set(LOADER_NAME PicoMultiLoader-rp2040)
else()
    set(LOADER_NAME pml2350)
endif()
set(LOADER_CLASSIC ${LOADER_NAME}-classic)

set(LOADER_WIFI 0)
if (PICO_CYW43_SUPPORTED AND NOT PICO_PLATFORM MATCHES "rp2040")
    set(LOADER_WIFI 1)
endif()

if (LOADER_WIFI)
    # The browser page is compiled in as a C string.
    set(INDEX_HTML ${CMAKE_CURRENT_LIST_DIR}/../common/web/index.html)
    set(INDEX_H ${CMAKE_CURRENT_BINARY_DIR}/generated/index_html.h)
    add_custom_command(
        OUTPUT ${INDEX_H}
        COMMAND ${CMAKE_COMMAND} -DIN=${INDEX_HTML} -DOUT=${INDEX_H} -P ${CMAKE_CURRENT_LIST_DIR}/embed.cmake
        DEPENDS ${INDEX_HTML} ${CMAKE_CURRENT_LIST_DIR}/embed.cmake
    )
    add_custom_target(index_html DEPENDS ${INDEX_H})
endif()

if (PICO_PLATFORM MATCHES "rp2040")
    # Linker script: the SDK's default flash layout, moved to where the loader lives (see the top).
    file(READ ${PICO_SDK_PATH}/src/rp2_common/pico_crt0/rp2040/memmap_default.ld memmap)
    set(LOADER_BASE 0x101C0000)
    string(REPLACE "INCLUDE \"pico_flash_region.ld\""
        "BOOT2(rx) : ORIGIN = 0x10000000, LENGTH = 256\n    FLASH(rx) : ORIGIN = ${LOADER_BASE}, LENGTH = 256k"
        memmap "${memmap}")
    # boot2 stays at the start of flash, where the bootrom loads it from
    string(REGEX REPLACE "(__boot2_end__ = [.];[ \t\r\n]*})[ \t]*> FLASH" "\\1 > BOOT2" memmap "${memmap}")
    if (NOT memmap MATCHES "> BOOT2")
        message(FATAL_ERROR "Could not place .boot2 in memmap_default.ld")
    endif()
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/memmap_pml.ld "${memmap}")

    # Our boot2 enters the loader rather than the app
    pico_define_boot_stage2(pml_boot2 ${CMAKE_CURRENT_LIST_DIR}/boot/boot2_pml.S)
    target_include_directories(pml_boot2 PRIVATE ${PICO_SDK_PATH}/src/rp2040/boot_stage2)
    target_compile_definitions(pml_boot2 PRIVATE PML_LOADER_BASE=${LOADER_BASE})
else()
    # Resources in flash: the Wi-Fi firmware and the web page are linked into the last 256 KB of
    # flash instead of RAM. The section isn't loaded (INFO); add_res.py appends it to the .uf2 as
    # blocks the stub skips, and the loader copies them into flash (uf2_image.c).
    set(RES_BASE 0x103C0000) # the last 256 KB of the Pico 2 W's 4 MB
    file(READ ${PICO_SDK_PATH}/src/rp2_common/pico_crt0/rp2350/memmap_no_flash.ld memmap)
    string(REPLACE "SCRATCH_Y(rwx) : ORIGIN = 0x20081000, LENGTH = 4k"
        "SCRATCH_Y(rwx) : ORIGIN = 0x20081000, LENGTH = 4k\n    PMLRES(r) : ORIGIN = ${RES_BASE}, LENGTH = 256k"
        memmap "${memmap}")
    string(REPLACE "SECTIONS\n{"
        "SECTIONS\n{\n    .pml_res (INFO) : { KEEP(*(.pml_res.0hdr)) KEEP(*(.pml_res*)) } > PMLRES\n"
        memmap "${memmap}")
    if (NOT memmap MATCHES "PMLRES[(]r[)]" OR NOT memmap MATCHES "> PMLRES")
        message(FATAL_ERROR "Could not add the resource section to memmap_no_flash.ld")
    endif()
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/memmap_pml.ld "${memmap}")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()

foreach(LOADER ${LOADER_NAME} ${LOADER_CLASSIC})
    if (LOADER STREQUAL LOADER_CLASSIC)
        set(UI_SOURCES src/ui_classic.c)
        set(UI_CLASSIC 1)
    else()
        set(UI_SOURCES src/ui.c src/apps.c src/look.c src/shell.c src/viewer.c)
        set(UI_CLASSIC 0)
    endif()
    add_executable(${LOADER}
        src/main.c
        src/clock.c
        src/config.c
        src/fileops.c
        src/keyboard.c
        src/lcd.c
        src/sdcard.c
        src/uf2_image.c
        src/usb_descriptors.c
        src/usb_drive.c
        lib/fatfs/ff.c
        lib/fatfs/ffunicode.c
        ${UI_SOURCES}
    )

    target_include_directories(${LOADER} PRIVATE
        src
        lib/fatfs
        ${CMAKE_CURRENT_LIST_DIR}/../common
    )

    target_compile_definitions(${LOADER} PRIVATE
        PICOMULTILOADER_WIFI=${LOADER_WIFI}
        PML_UI_CLASSIC=${UI_CLASSIC}
        PML_LOADER_FILE="/${LOADER}.uf2"
        PICO_STDIO_UART=1
        PICO_PRINTF_SUPPORT_FLOAT=0
        PICO_PRINTF_SUPPORT_EXPONENTIAL=0
        # The heap is small: a failed malloc must return NULL (callers handle it), not panic.
        PICO_MALLOC_PANIC=0
    )

    target_compile_options(${LOADER} PRIVATE -Wall -Wno-unused-function)

    target_link_libraries(${LOADER}
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
        target_sources(${LOADER} PRIVATE src/webserver.c src/dhcpserver.c src/timesync.c)
        add_dependencies(${LOADER} index_html)
        target_include_directories(${LOADER} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
        target_compile_definitions(${LOADER} PRIVATE CYW43_HOST_NAME="picocalc")
        target_link_libraries(${LOADER}
            pico_cyw43_arch_lwip_poll
            pico_lwip_mdns
            pico_rand
        )
    endif()

    pico_set_linker_script(${LOADER} ${CMAKE_CURRENT_BINARY_DIR}/memmap_pml.ld)
    if (PICO_PLATFORM MATCHES "rp2040")
        pico_set_boot_stage2(${LOADER} pml_boot2)
    else()
        # The loader runs from RAM; the boot stub loads it from the SD card.
        pico_set_binary_type(${LOADER} no_flash)
        target_compile_definitions(${LOADER} PRIVATE
            PML_RES_BASE=${RES_BASE}
            "PML_RES_ATTR=__attribute__((section(\".pml_res.data\"),aligned(4)))"
            "CYW43_RESOURCE_ATTRIBUTE=__attribute__((section(\".pml_res.fw\"),aligned(4)))")
    endif()

    pico_enable_stdio_uart(${LOADER} 1)
    pico_enable_stdio_usb(${LOADER} 0)
    target_link_options(${LOADER} PRIVATE -Wl,--print-memory-usage)
    pico_add_extra_outputs(${LOADER})
    if (NOT PICO_PLATFORM MATCHES "rp2040")
        # after pico_add_extra_outputs' own POST_BUILD steps, which write the .uf2
        set(RES_BIN ${CMAKE_CURRENT_BINARY_DIR}/${LOADER}_res.bin)
        add_custom_command(TARGET ${LOADER} POST_BUILD
            COMMAND ${CMAKE_OBJCOPY} --dump-section .pml_res=${RES_BIN} $<TARGET_FILE:${LOADER}>
                ${CMAKE_CURRENT_BINARY_DIR}/${LOADER}_res_dump.elf
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/add_res.py ${RES_BIN} ${RES_BASE}
                $<TARGET_FILE_DIR:${LOADER}>/${LOADER}.uf2
            VERBATIM)
    endif()
endforeach()

if (NOT PICO_PLATFORM MATCHES "rp2040")
    # Boot stub: the first 8 KB of flash. Its RAM is the scratch banks, so it can load the loader
    # into main RAM.
    set(STUB_NAME PicoMultiLoader-rp2350)
    add_executable(${STUB_NAME} stub/stub.c)
    target_include_directories(${STUB_NAME} PRIVATE src)
    target_compile_definitions(${STUB_NAME} PRIVATE PICO_NO_BINARY_INFO=1)
    target_compile_options(${STUB_NAME} PRIVATE -Wall -Os)
    target_link_libraries(${STUB_NAME} pico_stdlib hardware_i2c hardware_spi)
    pico_minimize_runtime(${STUB_NAME})
    pico_enable_stdio_uart(${STUB_NAME} 0)
    pico_enable_stdio_usb(${STUB_NAME} 0)
    file(READ ${PICO_SDK_PATH}/src/rp2_common/pico_crt0/rp2350/memmap_default.ld memmap)
    string(REPLACE "INCLUDE \"pico_flash_region.ld\""
        "FLASH(rx) : ORIGIN = 0x10000000, LENGTH = 0x1E00" memmap "${memmap}")
    string(REPLACE "RAM(rwx) : ORIGIN =  0x20000000, LENGTH = 512k"
        "RAM(rwx) : ORIGIN = 0x20080000, LENGTH = 3k" memmap "${memmap}")
    string(REPLACE "SCRATCH_X(rwx) : ORIGIN = 0x20080000, LENGTH = 4k"
        "SCRATCH_X(rwx) : ORIGIN = 0x20080C00, LENGTH = 1k" memmap "${memmap}")
    if (NOT memmap MATCHES "0x20080C00" OR NOT memmap MATCHES "0x1E00")
        message(FATAL_ERROR "Could not adapt memmap_default.ld for the stub")
    endif()
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/memmap_stub.ld "${memmap}")
    pico_set_linker_script(${STUB_NAME} ${CMAKE_CURRENT_BINARY_DIR}/memmap_stub.ld)
    # Partition table: apps from 8 KB to the end of flash, like uf2loader
    pico_embed_pt_in_binary(${STUB_NAME} ${CMAKE_CURRENT_LIST_DIR}/stub/pt.json)
    # "absolute": BOOTSEL writes it to the start of flash whatever partition table is there
    pico_set_uf2_family(${STUB_NAME} absolute)
    target_link_options(${STUB_NAME} PRIVATE -Wl,--print-memory-usage)
    pico_add_extra_outputs(${STUB_NAME})

    # The stub's RISC-V half (stub/stub_riscv.c) in its last 512 bytes, so RISC-V apps start too:
    # add_riscv_twin.py links it into the ARM stub's block loop and rewrites the .bin and .uf2.
    set(STUB_TWIN_OFFSET 0x1E00)
    file(GLOB RISCV_GCC_HINTS "$ENV{LOCALAPPDATA}/Arduino15/packages/rp2040/tools/pqt-gcc-riscv/*/bin")
    find_program(RISCV_GCC NAMES riscv32-unknown-elf-gcc riscv32-unknown-elf-gcc.exe HINTS ${RISCV_GCC_HINTS})
    if (NOT RISCV_GCC)
        message(FATAL_ERROR "riscv32-unknown-elf-gcc not found (set RISCV_GCC): the stub needs it for its RISC-V half")
    endif()
    string(REGEX REPLACE "gcc(\\.exe)?$" "objcopy\\1" RISCV_OBJCOPY "${RISCV_GCC}")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(TWIN_ELF ${CMAKE_CURRENT_BINARY_DIR}/stub_riscv.elf)
    set(TWIN_BIN ${CMAKE_CURRENT_BINARY_DIR}/stub_riscv.bin)
    math(EXPR TWIN_BASE "0x10000000 + ${STUB_TWIN_OFFSET}" OUTPUT_FORMAT HEXADECIMAL)
    add_custom_command(OUTPUT ${TWIN_BIN}
        COMMAND ${RISCV_GCC} -march=rv32imac_zicsr -mabi=ilp32 -Os -Wall -Werror -ffreestanding
            -nostdlib -nostartfiles -fno-pic -mno-relax -T ${CMAKE_CURRENT_LIST_DIR}/stub/stub_riscv.ld
            -Wl,--defsym=TWIN_BASE=${TWIN_BASE} -Wl,--gc-sections -Wl,-e,twin_main
            -o ${TWIN_ELF} ${CMAKE_CURRENT_LIST_DIR}/stub/stub_riscv.c
        COMMAND ${RISCV_OBJCOPY} -O binary -j .text ${TWIN_ELF} ${TWIN_BIN}
        DEPENDS stub/stub_riscv.c stub/stub_riscv.ld
        COMMENT "Building the boot stub's RISC-V half")
    add_custom_target(stub_riscv_twin DEPENDS ${TWIN_BIN})
    add_dependencies(${STUB_NAME} stub_riscv_twin)
    set_property(TARGET ${STUB_NAME} APPEND PROPERTY LINK_DEPENDS ${TWIN_BIN})   # relink: redo the splice
    # after pico_add_extra_outputs' own POST_BUILD steps, which write the .bin and .uf2
    add_custom_command(TARGET ${STUB_NAME} POST_BUILD
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/stub/add_riscv_twin.py
            $<TARGET_FILE_DIR:${STUB_NAME}>/${STUB_NAME}.bin ${TWIN_BIN} ${STUB_TWIN_OFFSET}
            $<TARGET_FILE_DIR:${STUB_NAME}>/${STUB_NAME}.bin $<TARGET_FILE_DIR:${STUB_NAME}>/${STUB_NAME}.uf2
        VERBATIM)
endif()
