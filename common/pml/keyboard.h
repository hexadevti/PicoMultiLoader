// keyboard.h - the PicoCalc's STM32 keyboard controller (I2C 0x1F)
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Key codes reported by the PicoCalc keyboard firmware
enum {
    KEY_NONE      = 0,
    KEY_CTRL_C    = 0x03, // Ctrl + a letter: the letter & 0x1F
    KEY_CTRL_V    = 0x16,
    KEY_CTRL_X    = 0x18,
    KEY_BACKSPACE = 0x08,
    KEY_TAB       = 0x09,
    KEY_ENTER     = 0x0A,
    KEY_F1        = 0x81,
    KEY_F2        = 0x82,
    KEY_F3        = 0x83,
    KEY_F4        = 0x84,
    KEY_F5        = 0x85,
    KEY_F6        = 0x86, // F6..F10: Shift + F1..F5
    KEY_F7        = 0x87,
    KEY_F8        = 0x88,
    KEY_F9        = 0x89,
    KEY_F10       = 0x90, // not 0x8A
    KEY_F10_ALT   = 0x8A, // in case a keyboard firmware sends this instead
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

/** True if a key is waiting for kbd_poll() (it is kept for it): for long jobs to give way. */
bool kbd_waiting(void);

/** True if the last kbd_init() / kbd_poll() reached the keyboard. */
bool kbd_present(void);

/**
 * At power-on: watch the keyboard for up to `window_ms` and return true as soon as Up, F1 or F5 is
 * reported pressed or held (the "open the menu" keys).
 */
bool kbd_boot_key_held(uint32_t window_ms);

/** Return the next pressed key (press and auto-repeat events), or KEY_NONE. */
int kbd_poll(void);

/** Battery charge in percent (0..100), or -1. Sets *charging if non-NULL. */
int kbd_battery(bool *charging);

/** Set the LCD backlight (0..255). */
void kbd_set_lcd_backlight(uint8_t level);

// ---- the platform's I2C to the keyboard controller (kbd_bus.c) ----
void kbd_bus_init(void);
/** Read a register (2 bytes), with the turnaround the STM32 needs. */
bool kbd_bus_read(uint8_t reg, uint8_t out[2]);
bool kbd_bus_write(uint8_t reg, uint8_t value);
