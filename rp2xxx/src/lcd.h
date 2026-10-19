// lcd.h - PicoCalc 320x320 ST7365P panel with a 16-colour RAM framebuffer and simple drawing.
// The framebuffer holds palette indices (4 bits per pixel, 50 KB); lcd_flush() expands them to
// RGB565 on the way out, so the whole UI fits in the RP2040's RAM.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "board.h"

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

typedef uint8_t lcd_color_t; // palette index 0..15

typedef enum {
    FONT_SMALL, // 6x10
    FONT_LARGE, // 7x13
} lcd_font_t;

/** Set up SPI (once) and send the panel init sequence. */
bool lcd_init(void);

/** Set palette entry `index` (0..15) to an RGB565 colour. */
void lcd_set_palette(lcd_color_t index, uint16_t rgb565);

/** Copy the framebuffer to the panel. */
void lcd_flush(void);

/** Copy rows y..y+h-1 of the framebuffer to the panel. */
void lcd_flush_rows(int y, int h);

void lcd_fill_rect(int x, int y, int w, int h, lcd_color_t color);
void lcd_hline(int x, int y, int w, lcd_color_t color);

/** Draw text with a transparent background. Returns the x after the last glyph. */
int lcd_text(int x, int y, const char *s, lcd_color_t color, lcd_font_t font);

int lcd_font_width(lcd_font_t font);
int lcd_font_height(lcd_font_t font);

/** Print the framebuffer on the console as run-length hex rows (Ctrl+P, used for screenshots). */
void lcd_dump(void);
