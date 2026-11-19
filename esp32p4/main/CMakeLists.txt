# The menus and the drivers they share with the RP2040/RP2350 build are in common/pml; this folder
# has the ESP32-P4 side (plat.h). Both menus are built in: ui_dispatch.c picks one at switch-on.
get_filename_component(PML ../../common/pml ABSOLUTE)
get_filename_component(APPS ../../apps ABSOLUTE)

# The bundled apps (apps/), built into the Tools tab (builtin.c): the same sources as the Picos'
# separate .uf2 apps, on their shared library (apps/sdk/pmlapp.c) in its PML_BUILTIN mode.
set(BUILTIN_SRCS "${APPS}/sdk/pmlapp.c" "${APPS}/image-editor/image.c" "${APPS}/music-player/player.c"
                 "${APPS}/calculator/calc.c" "audio_p4.c" "${PML}/builtin.c")

idf_component_register(SRCS "main.c" "ui_dispatch.c" "kbd_bus.c" "lcd_bus.c" "sdcard.c" "uf2_image.c"
                            "image_flash.c" "usb_drive.c" "webserver.c" "timesync.c"
                            "${PML}/apps.c" "${PML}/clock.c" "${PML}/config.c" "${PML}/fileops.c"
                            "${PML}/keyboard.c" "${PML}/lcd.c" "${PML}/look.c" "${PML}/shell.c"
                            "${PML}/ui.c" "${PML}/ui_classic.c" "${PML}/viewer.c" "${PML}/editor.c"
                            ${BUILTIN_SRCS}
                    INCLUDE_DIRS "." "../../common" "${PML}" "${APPS}/sdk"
                    EMBED_TXTFILES "../../common/web/index.html"
                    REQUIRES app_update bootloader_support esp_app_format esp_partition
                             fatfs sdmmc esp_driver_sdspi esp_driver_spi esp_driver_i2c esp_driver_gpio esp_driver_i2s
                             esp_lcd esp_hw_support esp_timer nvs_flash hal esp_system esp_rom espressif__esp_tinyusb
                             esp_netif esp_event esp_http_server espressif__esp_wifi_remote espressif__esp_hosted
                             espressif__mdns lwip)

target_compile_definitions(${COMPONENT_LIB} PRIVATE PICOMULTILOADER_WIFI=1)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function)

# Each menu's ui_* functions get their own names; ui_dispatch.c calls one set or the other
set(UI_API ui_boot ui_boot_check ui_init ui_key ui_redraw ui_tick ui_set_battery ui_backlight)
foreach(MENU gui uic)
    set(defs "")
    foreach(fn ${UI_API})
        string(REPLACE "ui_" "${MENU}_" renamed ${fn})
        list(APPEND defs "${fn}=${renamed}")
    endforeach()
    if (MENU STREQUAL gui)
        set_source_files_properties(${PML}/ui.c PROPERTIES COMPILE_DEFINITIONS "${defs}")
    else()
        set_source_files_properties(${PML}/ui_classic.c PROPERTIES COMPILE_DEFINITIONS "${defs}")
    endif()
endforeach()

# Each built-in app: its name and settings folder, its main() renamed for builtin.c, and the
# Pico SDK calls it makes (apps/sdk/builtin/pico/stdlib.h)
foreach(app "image-editor/image.c;image;Image Editor;ImageEditor"
            "music-player/player.c;music;Music Player;MusicPlayer"
            "calculator/calc.c;calc;Calculator;Calculator")
    list(GET app 0 src)
    list(GET app 1 id)
    list(GET app 2 name)
    list(GET app 3 folder)
    set_source_files_properties(${APPS}/${src} PROPERTIES
        COMPILE_DEFINITIONS "PML_BUILTIN=1;APP_NAME=\"${name}\";APP_VERSION=\"1.0.0\";APP_DIR=\"/pml/${folder}\";main=builtin_${id}_main"
        INCLUDE_DIRECTORIES "${APPS}/sdk/builtin"
        COMPILE_OPTIONS "-Wno-unused-variable;-Wno-format-truncation")
endforeach()
set_source_files_properties(${APPS}/sdk/pmlapp.c ${PML}/builtin.c PROPERTIES COMPILE_DEFINITIONS "PML_BUILTIN=1")
