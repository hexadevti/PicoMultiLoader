// lcd.c - the 16-colour framebuffer and drawing (lcd.h). The panel driver of each platform
// (lcd_bus.c) sends it: lcd_init() and lcd_flush_rows(), with lcd_expand_rows() for the colours.
#include "lcd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "uifont.h"

static uint8_t s_fb[LCD_W * LCD_H / 2]; // two pixels per byte: low nibble = even x
static uint16_t s_palette[16];

#if PML_LCD8
// The 8-bit mode for the built-in apps (lcd8_begin()): one byte per pixel, 256 colours.
static uint8_t *s_fb8;
static uint16_t s_pal8[256];

bool lcd8_begin(void)
{
    if (!s_fb8) {
        s_fb8 = calloc(LCD_W, LCD_H);
        if (!s_fb8) {
            return false;
        }
    }
    memset(s_pal8, 0, sizeof(s_pal8));
    memcpy(s_pal8, s_palette, sizeof(s_palette));
    return true;
}

void lcd8_end(void)
{
    free(s_fb8);
    s_fb8 = NULL;
}

uint8_t *lcd8_row(int y)
{
    return s_fb8 ? &s_fb8[y * LCD_W] : NULL;
}
#define FB8 s_fb8
#else
#define FB8 ((uint8_t *)NULL)
#endif

void lcd_set_palette(lcd_color_t index, uint16_t rgb565)
{
#if PML_LCD8
    s_pal8[index] = rgb565;
#endif
    if (index < 16) {
        s_palette[index] = rgb565;
    }
}

uint16_t lcd_get_palette(lcd_color_t index)
{
#if PML_LCD8
    if (s_fb8) {
        return s_pal8[index];
    }
#endif
    return s_palette[index & 15];
}

const uint8_t *lcd_row(int y)
{
    if (FB8) {
        return &FB8[y * LCD_W];
    }
    return &s_fb[y * (LCD_W / 2)];
}

void lcd_expand_rows(uint16_t *out, int y, int rows)
{
#if PML_LCD8
    if (s_fb8) {
        const uint8_t *p = &s_fb8[y * LCD_W];
        for (int i = 0; i < rows * LCD_W; i++) {
            out[i] = s_pal8[p[i]];
        }
        return;
    }
#endif
    const uint8_t *src = &s_fb[y * (LCD_W / 2)];
    for (int i = 0; i < rows * LCD_W / 2; i++) {
        uint8_t b = src[i];
        *out++ = s_palette[b & 15];
        *out++ = s_palette[b >> 4];
    }
}

void lcd_flush(void)
{
    lcd_flush_rows(0, LCD_H);
}

static inline void put_pixel(int x, int y, lcd_color_t c)
{
    if (FB8) {
        FB8[y * LCD_W + x] = c;
        return;
    }
    uint8_t *p = &s_fb[y * (LCD_W / 2) + (x >> 1)];
    *p = (x & 1) ? (uint8_t)((*p & 0x0F) | (c << 4)) : (uint8_t)((*p & 0xF0) | (c & 15));
}

void lcd_pixel(int x, int y, lcd_color_t color)
{
    if (x >= 0 && x < LCD_W && y >= 0 && y < LCD_H) {
        put_pixel(x, y, color);
    }
}

void lcd_fill_rect(int x, int y, int w, int h, lcd_color_t color)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > LCD_W ? LCD_W : x + w, y1 = y + h > LCD_H ? LCD_H : y + h;
    if (x0 >= x1) {
        return;
    }
    if (FB8) {
        for (int j = y0; j < y1; j++) {
            memset(&FB8[j * LCD_W + x0], color, x1 - x0);
        }
        return;
    }
    uint8_t both = (uint8_t)((color & 15) * 0x11);
    for (int j = y0; j < y1; j++) {
        int i = x0;
        if (i & 1) {
            put_pixel(i++, j, color);
        }
        int full = (x1 - i) / 2;
        memset(&s_fb[j * (LCD_W / 2) + i / 2], both, full);
        i += 2 * full;
        if (i < x1) {
            put_pixel(i, j, color);
        }
    }
}

void lcd_hline(int x, int y, int w, lcd_color_t color)
{
    lcd_fill_rect(x, y, w, 1, color);
}

void lcd_checker(int y, int h, lcd_color_t color)
{
    if (FB8) {
        for (int j = y < 0 ? 0 : y; j < y + h && j < LCD_H; j++) {
            for (int i = j & 1; i < LCD_W; i += 2) {
                FB8[j * LCD_W + i] = color;
            }
        }
        return;
    }
    uint8_t c = color & 15;
    for (int j = y < 0 ? 0 : y; j < y + h && j < LCD_H; j++) {
        // even rows: even pixels (low nibbles); odd rows: odd pixels (high nibbles)
        uint8_t keep = (j & 1) ? 0x0F : 0xF0, set = (j & 1) ? (uint8_t)(c << 4) : c;
        uint8_t *p = &s_fb[j * (LCD_W / 2)];
        for (int i = 0; i < LCD_W / 2; i++) {
            p[i] = (uint8_t)((p[i] & keep) | set);
        }
    }
}

int lcd_font_width(lcd_font_t font)
{
    return font == FONT_SMALL ? 6 : 7;
}

int lcd_font_height(lcd_font_t font)
{
    return font == FONT_SMALL ? 10 : 13;
}

int lcd_text(int x, int y, const char *s, lcd_color_t color, lcd_font_t font)
{
    const uint8_t *glyphs = font == FONT_SMALL ? UIFONT_6X10 : UIFONT_7X13;
    const int fw = lcd_font_width(font), fh = lcd_font_height(font);
    for (; *s; s++, x += fw) {
        uint8_t ch = (uint8_t)*s;
        if (ch < 0x20 || ch > 0x7E) {
            ch = '?';
        }
        const uint8_t *g = &glyphs[(ch - 0x20) * fh];
        for (int r = 0; r < fh; r++) {
            int py = y + r;
            if (py < 0 || py >= LCD_H) {
                continue;
            }
            for (int b = 0; b < 8; b++) {
                int px = x + b;
                if ((g[r] & (0x80 >> b)) && px >= 0 && px < LCD_W) {
                    put_pixel(px, py, color);
                }
            }
        }
    }
    return x;
}

static inline uint16_t pixel_rgb(int x, int y)
{
#if PML_LCD8
    if (s_fb8) {
        return s_pal8[s_fb8[y * LCD_W + x]];
    }
#endif
    uint8_t b = s_fb[y * (LCD_W / 2) + (x >> 1)];
    return s_palette[(x & 1) ? b >> 4 : b & 15];
}

void lcd_dump(void)
{
    // One line per row: "FB <y> " then runs of "<rgb565 hex>" or "<rgb565 hex>*<count>".
    for (int y = 0; y < LCD_H; y++) {
        printf("FB %d ", y);
        for (int x = 0; x < LCD_W;) {
            uint16_t c = pixel_rgb(x, y);
            int n = 1;
            while (x + n < LCD_W && pixel_rgb(x + n, y) == c) {
                n++;
            }
            if (n > 1) {
                printf("%04x*%d,", c, n);
            } else {
                printf("%04x,", c);
            }
            x += n;
        }
        printf("\n");
        if ((y & 7) == 7) {
            sleep_ms(1); // a busy screen takes seconds to print: let the idle task (watchdog) run
        }
    }
    printf("FB end\n");
    fflush(stdout);
}
