# The bundled PicoMultiLoader apps (Music Player, Text Editor, Image Editor, Calculator), built
# apart from the loader for the PicoCalc with a Raspberry Pi Pico (RP2040) or Pico 2 / 2 W (RP2350).
#
#   cmake -B build-rp2040 -G Ninja -DPICO_BOARD=pico
#   cmake -B build-rp2350 -G Ninja -DPICO_BOARD=pico2
#   cmake --build build-rp2040
#   cmake --build build-rp2350
#
# Each app is a normal flash build. Both builds put their files in dist/ (APPS_DIST), one folder
# per app with its app.ini and icon.bmp: copy those folders to the root of the SD card.
#
# To add an app: make a folder with its sources, app.ini and icon.bmp, and a CMakeLists.txt
# that calls pml_app() (see calculator/CMakeLists.txt), then add_subdirectory() it below.
cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type")
endif()

if (NOT PICO_BOARD)
    set(PICO_BOARD pico2 CACHE STRING "Board type")
endif()

set(PICO_NO_COPRO_DIS 1)

include(${CMAKE_CURRENT_LIST_DIR}/../rp2xxx/pico_sdk_import.cmake)

project(pml_apps C CXX ASM)

set(PICOTOOL_EXE "" CACHE FILEPATH "picotool executable to use (empty: let the SDK find or build one)")
if (PICOTOOL_EXE)
    add_executable(picotool IMPORTED GLOBAL)
    set_target_properties(picotool PROPERTIES IMPORTED_LOCATION "${PICOTOOL_EXE}")
endif()

pico_sdk_init()

if (PICO_PLATFORM MATCHES "rp2040")
    set(PML_CHIP rp2040)
else()
    set(PML_CHIP rp2350)
endif()

set(APPS_DIST ${CMAKE_CURRENT_LIST_DIR}/dist CACHE PATH "Where the SD card folders of the apps go")

# The shared app library (sdk/pmlapp.h) on top of the loader's drivers.
set(PML_COMMON ${CMAKE_CURRENT_LIST_DIR}/../common)
set(PML_RP2XXX ${CMAKE_CURRENT_LIST_DIR}/../rp2xxx)
add_library(pml_app_sdk INTERFACE)
target_sources(pml_app_sdk INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/sdk/pmlapp.c
    ${CMAKE_CURRENT_LIST_DIR}/sdk/lcd8.c
    ${CMAKE_CURRENT_LIST_DIR}/sdk/audio.c
    ${PML_COMMON}/pml/keyboard.c
    ${PML_COMMON}/pml/look.c
    ${PML_COMMON}/pml/config.c
    ${PML_RP2XXX}/src/kbd_bus.c
    ${PML_RP2XXX}/src/lcd_bus.c
    ${PML_RP2XXX}/src/sdcard.c
    ${PML_RP2XXX}/lib/fatfs/ff.c
    ${PML_RP2XXX}/lib/fatfs/ffunicode.c
)
target_include_directories(pml_app_sdk INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/sdk
    ${PML_COMMON}/pml
    ${PML_COMMON}
    ${PML_RP2XXX}/src
    ${PML_RP2XXX}/lib/fatfs
)
target_link_libraries(pml_app_sdk INTERFACE
    pico_stdlib
    hardware_spi
    hardware_i2c
    hardware_dma
    hardware_pwm
    hardware_watchdog
)

# pml_app(<target> <SD folder> SOURCES <files...> [LIBS <libraries...>])
#
# Builds <target>-<chip>.uf2 and copies it, with the folder's app.ini and icon.bmp, to
# ${APPS_DIST}/<SD folder>/. The program name and version in binary_info (shown on the loader's
# details screen) come from app.ini, so there is one place to change them.
function(pml_app TARGET FOLDER)
    cmake_parse_arguments(APP "" "" "SOURCES;LIBS" ${ARGN})
    set(INI ${CMAKE_CURRENT_SOURCE_DIR}/app.ini)
    file(STRINGS ${INI} NAME_LINE REGEX "^name[ \t]*=")
    file(STRINGS ${INI} VERSION_LINE REGEX "^version[ \t]*=")
    string(REGEX REPLACE "^name[ \t]*=[ \t]*([^;]*[^; \t]).*" "\\1" APP_NAME "${NAME_LINE}")
    string(REGEX REPLACE "^version[ \t]*=[ \t]*([^; \t]+).*" "\\1" APP_VERSION "${VERSION_LINE}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${INI})

    set(EXE ${TARGET}-${PML_CHIP})
    add_executable(${EXE} ${APP_SOURCES})
    target_link_libraries(${EXE} pml_app_sdk ${APP_LIBS})
    target_compile_definitions(${EXE} PRIVATE
        APP_NAME="${APP_NAME}"
        APP_VERSION="${APP_VERSION}"
        APP_DIR="/${FOLDER}"
    )
    target_compile_options(${EXE} PRIVATE -Wall -Wno-unused-function)
    pico_set_program_name(${EXE} "${APP_NAME}")
    pico_set_program_version(${EXE} "${APP_VERSION}")
    pico_enable_stdio_uart(${EXE} 1)
    pico_enable_stdio_usb(${EXE} 0)
    target_link_options(${EXE} PRIVATE -Wl,--print-memory-usage)
    pico_add_extra_outputs(${EXE})

    set(OUT ${APPS_DIST}/${FOLDER})
    add_custom_command(TARGET ${EXE} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${OUT}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE_DIR:${EXE}>/${EXE}.uf2 ${OUT}/${EXE}.uf2
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${INI} ${CMAKE_CURRENT_SOURCE_DIR}/icon.bmp ${OUT}
        VERBATIM)
endfunction()

add_subdirectory(music-player)
add_subdirectory(text-editor)
add_subdirectory(image-editor)
add_subdirectory(calculator)
