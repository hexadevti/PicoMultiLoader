/*
 * PicoMultiLoader (RP2040 / RP2350 build) for the PicoCalc: browse the SD card on the LCD, pick a
 * .uf2, flash it into the app area and boot it. This is the firmware the chip boots (flashed once
 * through BOOTSEL, no other bootloader): at power-on it starts the installed app, or shows the
 * menu when Up, F1 or F5 is held, an app asked for it, or no app is installed.
 *
 * The serial console (PicoCalc USB-C, UART0) works as a second keyboard: arrows, PgUp/PgDn, Enter,
 * Backspace, Esc, F1, F4, F5.
 */
#include <stdio.h>
#include "pico/stdlib.h"
#include "keyboard.h"
#include "lcd.h"
#include "sdcard.h"
#include "uf2_image.h"
#include "ui.h"
#include "usb_drive.h"
#if PICOMULTILOADER_WIFI
#include "webserver.h"
#endif

#define POLL_US          20000
#define BATTERY_EVERY_US (10 * 1000 * 1000)
#define REPROBE_EVERY_US (1000 * 1000)
#define BACKLIGHT        200

// Read one console byte if one is waiting, else -1.
static int console_getc(void)
{
    int c = getchar_timeout_us(0);
    return c == PICO_ERROR_TIMEOUT ? -1 : c;
}

// Wait briefly for the rest of an escape sequence.
static int console_getc_wait(void)
{
    int c = getchar_timeout_us(20000);
    return c == PICO_ERROR_TIMEOUT ? -1 : c;
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

int main(void)
{
    stdio_uart_init_full(CONSOLE_UART, CONSOLE_BAUD, CONSOLE_TX_PIN, CONSOLE_RX_PIN);
    printf("\nPicoMultiLoader for %s\n", uf2_chip_name());
    uf2_boot_or_menu(); // returns only if the menu is wanted

    bool sd_ok = sdcard_mount();
    if (!lcd_init()) {
        printf("loader: LCD init failed\n");
    }
    if (kbd_init()) {
        kbd_set_lcd_backlight(BACKLIGHT);
    } else {
        printf("loader: no keyboard - is the PicoCalc switched on? Serial input still works.\n");
    }
    ui_init(sd_ok);
    update_battery();
    bool charging = false;
    int battery = kbd_battery(&charging);
    printf("loader: ready: SD %s, keyboard %s, battery %d%%%s\n", sd_ok ? "mounted" : "missing",
           kbd_present() ? "found" : "missing", battery, charging ? " (charging)" : "");

    uint64_t next_poll = 0;
    uint64_t next_battery = time_us_64() + BATTERY_EVERY_US;
    uint64_t next_probe = 0;
    for (;;) {
        // USB and Wi-Fi need servicing far more often than the keyboard
        usbdrive_task();
#if PICOMULTILOADER_WIFI
        webserver_poll();
#endif
        uint64_t now = time_us_64();
        if (now < next_poll) {
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
                printf("loader: keyboard found\n");
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
    }
}
