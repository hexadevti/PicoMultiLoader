// gfx.h - a small 2D drawing library on an RGB565 frame buffer: pixels, lines, rectangles,
// circles, triangles and text in two bitmap fonts. Everything is clipped to the screen (or to
// the rectangle set with gfx_clip()).
#pragma once

#include <stdbool.h>
#include <stdint.h>

/** An RGB565 colour from 8-bit red, green and blue. */
#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define C_BLACK   RGB(0, 0, 0)
#define C_WHITE   RGB(255, 255, 255)
#define C_RED     RGB(255, 80, 80)
#define C_GREEN   RGB(90, 220, 130)
#define C_BLUE    RGB(70, 130, 255)
#define C_YELLOW  RGB(255, 220, 60)
#define C_ORANGE  RGB(255, 160, 60)
#define C_CYAN    RGB(76, 195, 255)
#define C_MAGENTA RGB(200, 120, 255)
#define C_GRAY    RGB(122, 136, 163)
#define C_DKGRAY  RGB(51, 65, 92)

typedef enum {
    FONT_SMALL, // 6 x 10 pixels
    FONT_LARGE, // 7 x 13 pixels
} gfx_font_t;

void gfx_init(uint16_t *fb, int w, int h);

/** Limit drawing to a rectangle; gfx_noclip() draws on the whole screen again. */
void gfx_clip(int x, int y, int w, int h);
void gfx_noclip(void);

void gfx_clear(uint16_t c);
void gfx_pixel(int x, int y, uint16_t c);
void gfx_hline(int x, int y, int w, uint16_t c);
void gfx_vline(int x, int y, int h, uint16_t c);
void gfx_line(int x0, int y0, int x1, int y1, uint16_t c);
void gfx_rect(int x, int y, int w, int h, uint16_t c);
void gfx_fill_rect(int x, int y, int w, int h, uint16_t c);
void gfx_fill_round_rect(int x, int y, int w, int h, int r, uint16_t c);
void gfx_circle(int cx, int cy, int r, uint16_t c);
void gfx_fill_circle(int cx, int cy, int r, uint16_t c);
void gfx_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c);

/** Mix two colours: alpha 0 gives a, 255 gives b. */
uint16_t gfx_blend(uint16_t a, uint16_t b, int alpha);

/**
 * Draw text with its top-left corner at (x, y), each font pixel drawn as a scale x scale block.
 * Only the letters are drawn (the background shows through). '\n' starts a new line.
 * Returns the x after the last character.
 */
int gfx_text(int x, int y, const char *s, uint16_t c, gfx_font_t font, int scale);
int gfx_printf(int x, int y, uint16_t c, gfx_font_t font, int scale, const char *fmt, ...)
    __attribute__((format(printf, 6, 7)));
int gfx_text_width(const char *s, gfx_font_t font, int scale);
int gfx_font_height(gfx_font_t font, int scale);
