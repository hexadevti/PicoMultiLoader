// lcd.c - the 16-colour framebuffer and drawing (lcd.h). The panel driver of each platform
// (lcd_bus.c) sends it: lcd_init() and lcd_flush_rows(), with lcd_expand_rows() for the colours.
#include "lcd.h"

#include <stdio.h>
#include <string.h>
#include "uifont.h"

static uint8_t s_fb[LCD_W * LCD_H / 2]; // two pixels per byte: low nibble = even x
static uint16_t s_palette[16];

void lcd_set_palette(lcd_color_t index, uint16_t rgb565)
{
    s_palette[index & 15] = rgb565;
}

uint16_t lcd_get_palette(lcd_color_t index)
{
    return s_palette[index & 15];
}

const uint8_t *lcd_row(int y)
{
    return &s_fb[y * (LCD_W / 2)];
}

void lcd_expand_rows(uint16_t *out, int y, int rows)
{
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
    }
    printf("FB end\n");
    fflush(stdout);
}
