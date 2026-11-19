// lcd.h - PicoCalc 320x320 ST7365P panel with a 16-colour RAM framebuffer and simple drawing.
// The framebuffer holds palette indices (4 bits per pixel, 50 KB); lcd_flush() expands them to
// RGB565 on the way out, so the whole UI fits in the RP2040's RAM.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "board.h"

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

typedef uint8_t lcd_color_t; // palette index 0..15 (0..255 in the 8-bit mode)

typedef enum {
    FONT_SMALL, // 6x10
    FONT_LARGE, // 7x13
} lcd_font_t;

/** Set up SPI (once) and send the panel init sequence. */
bool lcd_init(void);

/** Set palette entry `index` (0..15) to an RGB565 colour. */
void lcd_set_palette(lcd_color_t index, uint16_t rgb565);

/** Palette entry `index` as RGB565. */
uint16_t lcd_get_palette(lcd_color_t index);

/** One framebuffer row: LCD_W / 2 bytes, two pixels per byte, the low nibble is the even x. */
const uint8_t *lcd_row(int y);

void lcd_pixel(int x, int y, lcd_color_t color);

/** Copy the framebuffer to the panel. */
void lcd_flush(void);

/** Copy rows y..y+h-1 of the framebuffer to the panel (the platform's lcd_bus.c). */
void lcd_flush_rows(int y, int h);

/** For lcd_flush_rows(): rows y..y+rows-1 as RGB565 pixels (native byte order). */
void lcd_expand_rows(uint16_t *out, int y, int rows);

void lcd_fill_rect(int x, int y, int w, int h, lcd_color_t color);
void lcd_hline(int x, int y, int w, lcd_color_t color);

/** Set every other pixel of rows y..y+h-1 to `color`, in a checkerboard. */
void lcd_checker(int y, int h, lcd_color_t color);

/** Draw text with a transparent background. Returns the x after the last glyph. */
int lcd_text(int x, int y, const char *s, lcd_color_t color, lcd_font_t font);

int lcd_font_width(lcd_font_t font);
int lcd_font_height(lcd_font_t font);

// The 8-bit mode (platforms with PML_LCD8, for the built-in apps): while it is on, the drawing calls
// work on one byte per pixel and a 256-entry palette (entries 0..15 start as the loader's), and
// lcd_row() returns LCD_W bytes. The 4-bit framebuffer is kept: redraw it after lcd8_end().
bool lcd8_begin(void);
void lcd8_end(void);
/** One row of the 8-bit framebuffer, or NULL when the mode is off. */
uint8_t *lcd8_row(int y);

/** Print the framebuffer on the console as run-length hex rows (Ctrl+P, used for screenshots). */
void lcd_dump(void);
