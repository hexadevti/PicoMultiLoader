// picocalc.h - the PicoCalc hardware for an app: the 320x320 screen, the keyboard, the battery
// and the way back to PicoMultiLoader's menu.
//
// This file, picocalc.c, gfx.c and main.c are the same in the RP2350 and ESP32-P4 start kits.
// Only port_rp2350.c / port_esp32p4.c differ: they implement the port_* calls at the end.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PC_W 320
#define PC_H 320

// Key codes sent by the PicoCalc keyboard. Letters, digits and symbols are their ASCII code
// (Shift is already applied by the keyboard).
enum {
    KEY_NONE      = 0,
    KEY_BACKSPACE = 0x08,
    KEY_TAB       = 0x09,
    KEY_ENTER     = 0x0A,
    KEY_F1        = 0x81,
    KEY_F2        = 0x82,
    KEY_F3        = 0x83,
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

/** Start the screen and the keyboard. Returns false if the screen could not be set up. */
bool pc_init(void);

/** The frame buffer: PC_W * PC_H pixels, RGB565, row by row. Draw here, then pc_flush(). */
uint16_t *pc_framebuffer(void);

/** Send the whole frame buffer to the screen (about 45 ms; returns when it is sent). */
void pc_flush(void);

/** The next key pressed (arrows and Backspace auto-repeat while held), or KEY_NONE. */
int pc_key(void);

/** Battery charge in percent (0..100), or -1 if the keyboard does not answer. */
int pc_battery(bool *charging);

/** Screen backlight, 0..255. */
void pc_backlight(uint8_t level);

uint32_t pc_millis(void);
void pc_sleep_ms(uint32_t ms);

/** "RP2350" or "ESP32-P4". */
const char *pc_board_name(void);

/** Restart into PicoMultiLoader's menu. Does not return. */
void pc_exit_to_menu(void);

// ---- the platform port (port_<board>.c) ----------------------------------------------------
bool port_lcd_init(void);
void port_lcd_send(const uint16_t *fb); // the whole PC_W x PC_H frame, native-endian RGB565
void port_kbd_init(void);
bool port_kbd_read(uint8_t reg, uint8_t out[2]);
bool port_kbd_write(uint8_t reg, uint8_t value);
uint64_t port_time_us(void);
void port_sleep_ms(uint32_t ms);
void port_exit_to_menu(void);
const char *port_board_name(void);
