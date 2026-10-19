/*
 * ESP32-P4 bin loader for the PicoCalc: browse the SD card on the LCD, pick a firmware .bin,
 * flash it into the "ota_0" partition and boot it. Hold Up, F1 or F5 while resetting (or call
 * binloader_reboot_to_menu() from loader_api.h) to come back here.
 *
 * The serial console works as a second keyboard: arrows, PgUp/PgDn, Enter, Backspace, Esc, F4, F5.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "keyboard.h"
#include "lcd.h"
#include "sdcard.h"
#include "ui.h"

static const char *TAG = "loader";

#define POLL_MS          20
#define BATTERY_EVERY_US (10 * 1000 * 1000)
#define REPROBE_EVERY_US (1000 * 1000)
#define BACKLIGHT        200

// Read one console byte if one is waiting, else -1.
static int console_getc(void)
{
    int c = getchar();
    if (c == EOF) {
        clearerr(stdin);
        return -1;
    }
    return c;
}

// Wait briefly for the rest of an escape sequence.
static int console_getc_wait(void)
{
    for (int i = 0; i < 10; i++) {
        int c = console_getc();
        if (c >= 0) {
            return c;
        }
        esp_rom_delay_us(2000);
    }
    return -1;
}

// Map terminal input (ANSI escape sequences) onto PicoCalc key codes.
static int console_key(void)
{
    int c = console_getc();
    switch (c) {
    case -1:
        return KEY_NONE;
    case '\r':
    case '\n':
        return KEY_ENTER;
    case 0x08:
    case 0x7F:
        return KEY_BACKSPACE;
    case 0x1B:
        break;
    case '\t':
        return KEY_TAB;
    case 0x10: // Ctrl+P: dump the screen for screenshots
        lcd_dump();
        return KEY_NONE;
    default:
        return c >= 0x20 && c < 0x7F ? c : KEY_NONE; // typed text (Wi-Fi settings)
    }

    int c1 = console_getc_wait();
    if (c1 != '[' && c1 != 'O') {
        return KEY_ESC;
    }
    int c2 = console_getc_wait();
    switch (c2) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    case 'P': return KEY_F1;
    case 'S': return KEY_F4; // ESC O S
    case 'T': return KEY_F5; // ESC O T (some terminals)
    default:
        break;
    }

    // ESC [ <number> ~
    int num = 0;
    for (int c = c2; c != '~'; c = console_getc_wait()) {
        if (c < '0' || c > '9' || num > 99) {
            return KEY_NONE;
        }
        num = num * 10 + (c - '0');
    }
    switch (num) {
    case 1: case 7: return KEY_HOME;
    case 4: case 8: return KEY_END;
    case 3: return KEY_DEL;
    case 5: return KEY_PAGE_UP;
    case 6: return KEY_PAGE_DOWN;
    case 11: return KEY_F1;
    case 14: return KEY_F4;
    case 15: return KEY_F5;
    default: return KEY_NONE;
    }
}

static void update_battery(void)
{
    bool charging = false;
    int pct = kbd_battery(&charging);
    if (pct >= 0) {
        ui_set_battery(pct, charging);
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    setvbuf(stdin, NULL, _IONBF, 0);

    bool sd_ok = sdcard_mount() == ESP_OK;
    if (lcd_init() != ESP_OK) {
        ESP_LOGE(TAG, "LCD init failed");
    }
    if (kbd_init() == ESP_OK) {
        kbd_set_lcd_backlight(BACKLIGHT);
    } else {
        ESP_LOGW(TAG, "No keyboard - is the PicoCalc switched on? Serial input still works.");
    }
    ui_init(sd_ok);
    update_battery();
    bool charging = false;
    int battery = kbd_battery(&charging);
    ESP_LOGI(TAG, "Ready: SD %s, keyboard %s, battery %d%%%s", sd_ok ? "mounted" : "missing",
             kbd_present() ? "found" : "missing", battery, charging ? " (charging)" : "");

    int64_t next_battery = esp_timer_get_time() + BATTERY_EVERY_US;
    int64_t next_probe = 0;
    for (;;) {
        int64_t now = esp_timer_get_time();

        if (kbd_present()) {
            int key;
            while ((key = kbd_poll()) != KEY_NONE) {
                ui_key(key);
            }
        } else if (now >= next_probe) {
            // The PicoCalc was switched on after us: the panel lost power, so set it up again.
            next_probe = now + REPROBE_EVERY_US;
            if (kbd_init() == ESP_OK) {
                ESP_LOGI(TAG, "Keyboard found");
                lcd_init();
                kbd_set_lcd_backlight(BACKLIGHT);
                ui_redraw();
                update_battery();
            }
        }

        int key;
        while ((key = console_key()) != KEY_NONE) {
            ui_key(key);
        }

        ui_tick();

        if (now >= next_battery && kbd_present()) {
            next_battery = now + BATTERY_EVERY_US;
            update_battery();
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}
