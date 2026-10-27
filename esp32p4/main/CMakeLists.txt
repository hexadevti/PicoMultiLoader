# The menus and the drivers they share with the RP2040/RP2350 build are in common/pml; this folder
# has the ESP32-P4 side (plat.h). Both menus are built in: ui_dispatch.c picks one at switch-on.
get_filename_component(PML ../../common/pml ABSOLUTE)

idf_component_register(SRCS "main.c" "ui_dispatch.c" "kbd_bus.c" "lcd_bus.c" "sdcard.c" "uf2_image.c"
                            "image_flash.c" "usb_drive.c" "webserver.c" "timesync.c"
                            "${PML}/apps.c" "${PML}/clock.c" "${PML}/config.c" "${PML}/fileops.c"
                            "${PML}/keyboard.c" "${PML}/lcd.c" "${PML}/look.c" "${PML}/shell.c"
                            "${PML}/ui.c" "${PML}/ui_classic.c" "${PML}/viewer.c"
                    INCLUDE_DIRS "." "../../common" "${PML}"
                    EMBED_TXTFILES "../../common/web/index.html"
                    REQUIRES app_update bootloader_support esp_app_format esp_partition
                             fatfs sdmmc esp_driver_sdspi esp_driver_spi esp_driver_i2c esp_driver_gpio
                             esp_lcd esp_hw_support esp_timer nvs_flash hal esp_system esp_rom espressif__esp_tinyusb
                             esp_netif esp_event esp_http_server espressif__esp_wifi_remote espressif__mdns lwip)

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
