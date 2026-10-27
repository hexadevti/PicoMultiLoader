/*
 * PicoMultiLoader (ESP32-P4 build) for the PicoCalc: browse the SD card on the LCD, pick a firmware
 * .bin, flash it into the "ota_0" partition and boot it. The custom bootloader starts the installed
 * app at reset, or this loader when the BOOT button, Up, F1 or F5 is held, an app asked for it
 * (picomultiloader_reboot_to_menu() in loader_api.h), or no app is installed.
 *
 * The serial console (USB-C) works as a second keyboard: arrows, PgUp/PgDn, Enter, Backspace, Esc,
 * F1-F5 and typing (the terminal).
 */
#include <stdio.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "keyboard.h"
#include "lcd.h"
#include "plat.h"
#include "sdcard.h"
#include "uf2_image.h"
#include "ui.h"
#include "usb_drive.h"
#include "webserver.h"

static const char *TAG = "loader";

#define POLL_US          20000
#define BATTERY_EVERY_US (10 * 1000 * 1000)
#define REPROBE_EVERY_US (1000 * 1000)

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
        sleep_us(2000);
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
    case KEY_CTRL_C:
    case KEY_CTRL_V:
    case KEY_CTRL_X:
        return c;
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
    case 'Q': return KEY_F2;
    case 'R': return KEY_F3;
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
    case 12: return KEY_F2;
    case 13: return KEY_F3;
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
    printf("\nPicoMultiLoader for %s\n", uf2_chip_name());

    bool sd_ok = sdcard_mount();
    if (!lcd_init()) {
        ESP_LOGE(TAG, "LCD init failed");
    }
    ui_boot(sd_ok);
    bool kbd_ok = kbd_init();
    if (kbd_ok) {
        kbd_set_lcd_backlight(ui_backlight());
    } else {
        ESP_LOGW(TAG, "No keyboard - is the PicoCalc switched on? Serial input still works.");
    }
    ui_boot_check("Keyboard", kbd_ok ? "OK" : "missing", kbd_ok);
    bool charging = false;
    int battery = kbd_battery(&charging);
    char pct[20];
    snprintf(pct, sizeof(pct), battery < 0 ? "unknown" : charging ? "%d%% charging" : "%d%%", battery);
    ui_boot_check("Battery", pct, battery >= 0);
    ui_init(sd_ok);
    update_battery();
    ESP_LOGI(TAG, "Ready: SD %s, keyboard %s, battery %d%%%s", sd_ok ? "mounted" : "missing",
             kbd_present() ? "found" : "missing", battery, charging ? " (charging)" : "");

    uint64_t next_poll = 0;
    uint64_t next_battery = time_us_64() + BATTERY_EVERY_US;
    uint64_t next_probe = 0;
    for (;;) {
        webserver_poll();
        uint64_t now = time_us_64();
        if (now < next_poll) {
            webserver_wait_until(next_poll); // USB and Wi-Fi run in their own tasks
            continue;
        }
        next_poll = now + POLL_US;

        if (kbd_present()) {
            int key;
            while ((key = kbd_poll()) != KEY_NONE) {
                ui_key(key);
            }
        } else if (now >= next_probe) {
            // The PicoCalc was switched on after us: the panel lost power, so set it up again.
            next_probe = now + REPROBE_EVERY_US;
            if (kbd_init()) {
                ESP_LOGI(TAG, "Keyboard found");
                lcd_init();
                kbd_set_lcd_backlight(ui_backlight());
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
    }
}
