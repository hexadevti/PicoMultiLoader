// lcd.h - PicoCalc 320x320 ST7365P panel with a RAM framebuffer and simple drawing
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "board.h"

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

typedef enum {
    FONT_SMALL, // 6x10
    FONT_LARGE, // 7x13
} lcd_font_t;

/** Initialise the SPI bus (once) and send the panel init sequence. */
esp_err_t lcd_init(void);

/** Copy the framebuffer to the panel. */
void lcd_flush(void);

void lcd_fill_rect(int x, int y, int w, int h, uint16_t color);
void lcd_hline(int x, int y, int w, uint16_t color);

/** Draw text with a transparent background. Returns the x after the last glyph. */
int lcd_text(int x, int y, const char *s, uint16_t color, lcd_font_t font);

int lcd_font_width(lcd_font_t font);
int lcd_font_height(lcd_font_t font);

/** Print the framebuffer on the console as run-length hex rows (Ctrl+P, used for screenshots). */
void lcd_dump(void);
