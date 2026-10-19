// keyboard.h - the PicoCalc's STM32 keyboard controller (I2C 0x1F)
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Key codes reported by the PicoCalc keyboard firmware
enum {
    KEY_NONE      = 0,
    KEY_BACKSPACE = 0x08,
    KEY_TAB       = 0x09,
    KEY_ENTER     = 0x0A,
    KEY_F1        = 0x81,
    KEY_F4        = 0x84,
    KEY_F5        = 0x85,
    KEY_ESC       = 0xB1,
    KEY_LEFT      = 0xB4,
    KEY_UP        = 0xB5,
    KEY_DOWN      = 0xB6,
    KEY_RIGHT     = 0xB7,
    KEY_HOME      = 0xD2,
    KEY_DEL       = 0xD4,
    KEY_END       = 0xD5,
    KEY_PAGE_UP   = 0xD6,
    KEY_PAGE_DOWN = 0xD7,
};

/** Probe the keyboard. Returns true if it answers (i.e. the PicoCalc is switched on). */
bool kbd_init(void);

/** True if the last kbd_init() / kbd_poll() reached the keyboard. */
bool kbd_present(void);

/** Return the next pressed key (press and auto-repeat events), or KEY_NONE. */
int kbd_poll(void);

/** Battery charge in percent (0..100), or -1. Sets *charging if non-NULL. */
int kbd_battery(bool *charging);

/** Set the LCD backlight (0..255). */
void kbd_set_lcd_backlight(uint8_t level);
