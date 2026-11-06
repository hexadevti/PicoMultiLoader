# PicoCalc start kit for the Raspberry Pi Pico 2 / Pico 2 W (RP2350), built with the Pico SDK.
#
#   cmake -B build -G Ninja
#   cmake --build build
#
# The SD card folder is written to dist/StartKit/ (app.ini, icon.bmp, startkit.uf2): copy it to
# the root of the card. The name and version come from app.ini, so they are set in one place.
cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type")
endif()
if (NOT PICO_BOARD)
    set(PICO_BOARD pico2 CACHE STRING "Board type") # pico2_w works too
endif()

include(pico_sdk_import.cmake)
project(startkit C CXX ASM)

set(PICOTOOL_EXE "" CACHE FILEPATH "picotool to use (empty: let the SDK find or build one)")
if (PICOTOOL_EXE)
    add_executable(picotool IMPORTED GLOBAL)
    set_target_properties(picotool PROPERTIES IMPORTED_LOCATION "${PICOTOOL_EXE}")
endif()

pico_sdk_init()

# name and version from app.ini
set(INI ${CMAKE_CURRENT_LIST_DIR}/app.ini)
file(STRINGS ${INI} NAME_LINE REGEX "^name[ \t]*=")
file(STRINGS ${INI} VERSION_LINE REGEX "^version[ \t]*=")
string(REGEX REPLACE "^name[ \t]*=[ \t]*([^;]*[^; \t]).*" "\\1" APP_NAME "${NAME_LINE}")
string(REGEX REPLACE "^version[ \t]*=[ \t]*([^; \t]+).*" "\\1" APP_VERSION "${VERSION_LINE}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${INI})

add_executable(startkit
    src/main.c
    src/gfx.c
    src/picocalc.c
    src/port_rp2350.c
)
target_compile_definitions(startkit PRIVATE APP_NAME="${APP_NAME}" APP_VERSION="${APP_VERSION}")
target_compile_options(startkit PRIVATE -Wall)
target_link_libraries(startkit
    pico_stdlib
    hardware_spi
    hardware_i2c
    hardware_dma
    hardware_watchdog
)

# shown on PicoMultiLoader's details screen
pico_set_program_name(startkit "${APP_NAME}")
pico_set_program_version(startkit "${APP_VERSION}")
pico_enable_stdio_uart(startkit 1)
pico_enable_stdio_usb(startkit 0)
pico_add_extra_outputs(startkit)

set(DIST ${CMAKE_CURRENT_LIST_DIR}/dist/StartKit)
add_custom_command(TARGET startkit POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DIST}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE_DIR:startkit>/startkit.uf2 ${DIST}/startkit.uf2
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${INI} ${CMAKE_CURRENT_LIST_DIR}/icon.bmp ${DIST}
    VERBATIM)
