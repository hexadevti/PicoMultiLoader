# PicoCalc start kit for the Waveshare ESP32-P4-WIFI6, built with ESP-IDF 5.5.
#
#   idf.py build
#
# The SD card folder is written to dist/StartKitP4/ (app.ini, icon.bmp, startkit-p4.bin): copy
# it to the root of the card. The version comes from app.ini (PicoMultiLoader's details screen
# shows it from the app descriptor).
cmake_minimum_required(VERSION 3.16)

file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/app.ini VERSION_LINE REGEX "^version[ \t]*=")
string(REGEX REPLACE "^version[ \t]*=[ \t]*([^; \t]+).*" "\\1" PROJECT_VER "${VERSION_LINE}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(startkit-p4)

set(DIST ${CMAKE_CURRENT_LIST_DIR}/dist/StartKitP4)
add_custom_target(dist ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DIST}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/startkit-p4.bin ${DIST}/startkit-p4.bin
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_LIST_DIR}/app.ini ${CMAKE_CURRENT_LIST_DIR}/icon.bmp ${DIST}
    DEPENDS gen_project_binary
    VERBATIM)
