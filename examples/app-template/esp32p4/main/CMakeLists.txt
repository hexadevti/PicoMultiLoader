# main.c, gfx.c and picocalc.c are the same as in the RP2350 start kit; port_esp32p4.c is the
# ESP32-P4 side.
idf_component_register(SRCS "main.c" "gfx.c" "picocalc.c" "port_esp32p4.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_lcd esp_driver_spi esp_driver_i2c esp_driver_gpio
                                esp_timer esp_rom esp_system hal soc)

# name and version from app.ini (the top-level CMakeLists.txt also uses the version)
set(INI ${CMAKE_CURRENT_LIST_DIR}/../app.ini)
file(STRINGS ${INI} NAME_LINE REGEX "^name[ \t]*=")
file(STRINGS ${INI} VERSION_LINE REGEX "^version[ \t]*=")
string(REGEX REPLACE "^name[ \t]*=[ \t]*([^;]*[^; \t]).*" "\\1" APP_NAME "${NAME_LINE}")
string(REGEX REPLACE "^version[ \t]*=[ \t]*([^; \t]+).*" "\\1" APP_VERSION "${VERSION_LINE}")

target_compile_definitions(${COMPONENT_LIB} PRIVATE APP_NAME="${APP_NAME}" APP_VERSION="${APP_VERSION}")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wno-unused-function)
