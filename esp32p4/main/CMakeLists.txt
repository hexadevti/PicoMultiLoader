idf_component_register(SRCS "loader_main.c" "sdcard.c" "image_flash.c" "keyboard.c" "lcd.c" "ui.c" "usb_drive.c"
                            "webserver.c"
                    INCLUDE_DIRS "." "../../common"
                    EMBED_TXTFILES "../../common/web/index.html"
                    REQUIRES app_update bootloader_support esp_app_format esp_partition
                             fatfs sdmmc esp_driver_sdspi esp_driver_spi esp_driver_i2c esp_driver_gpio
                             esp_lcd esp_hw_support esp_timer nvs_flash hal esp_system esp_rom espressif__esp_tinyusb
                             esp_netif esp_event esp_http_server espressif__esp_wifi_remote espressif__mdns)
