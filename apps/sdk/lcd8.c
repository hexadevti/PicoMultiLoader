// lcd8.c - the apps' framebuffer: the drawing calls of common/pml/lcd.h on 8 bits per pixel and a
// 256-entry palette (100 KB), sent by the loader's own panel driver (rp2xxx/src/lcd_bus.c).
// Entries 0..15 are the theme's roles (look.h), as in the loader; apps may use 16..255 freely
// (the image editor puts a colour cube there).
#include "lcd.h"

#include <stdio.h>
#include <string.h>
#include "uifont.h"

static uint8_t s_fb[LCD_W * LCD_H];
static uint16_t s_palette[256];

void lcd_set_palette(lcd_color_t index, uint16_t rgb565)
{
    s_palette[index] = rgb565;
}

uint16_t lcd_get_palette(lcd_color_t index)
{
    return s_palette[index];
}

// Unlike the loader's 4-bit lcd.c: one byte per pixel.
const uint8_t *lcd_row(int y)
{
    return &s_fb[y * LCD_W];
}

uint8_t *app_fb_row(int y)
{
    return &s_fb[y * LCD_W];
}

void lcd_expand_rows(uint16_t *out, int y, int rows)
{
    const uint8_t *src = &s_fb[y * LCD_W];
    for (int i = 0; i < rows * LCD_W; i++) {
        out[i] = s_palette[src[i]];
    }
}

void lcd_flush(void)
{
    lcd_flush_rows(0, LCD_H);
}

void lcd_pixel(int x, int y, lcd_color_t color)
{
    if (x >= 0 && x < LCD_W && y >= 0 && y < LCD_H) {
        s_fb[y * LCD_W + x] = color;
    }
}

void lcd_fill_rect(int x, int y, int w, int h, lcd_color_t color)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > LCD_W ? LCD_W : x + w, y1 = y + h > LCD_H ? LCD_H : y + h;
    for (int j = y0; j < y1 && x0 < x1; j++) {
        memset(&s_fb[j * LCD_W + x0], color, x1 - x0);
    }
}

void lcd_hline(int x, int y, int w, lcd_color_t color)
{
    lcd_fill_rect(x, y, w, 1, color);
}

void lcd_checker(int y, int h, lcd_color_t color)
{
    for (int j = y < 0 ? 0 : y; j < y + h && j < LCD_H; j++) {
        for (int i = j & 1; i < LCD_W; i += 2) {
            s_fb[j * LCD_W + i] = color;
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
            if (py < 0 || py >= LCD_H || !g[r]) {
                continue;
            }
            for (int b = 0; b < 8; b++) {
                int px = x + b;
                if ((g[r] & (0x80 >> b)) && px >= 0 && px < LCD_W) {
                    s_fb[py * LCD_W + px] = color;
                }
            }
        }
    }
    return x;
}

void lcd_dump(void)
{
    // The loader's format (tools/screenshots): "FB <y> " then runs of "<rgb565 hex>[*<count>],".
    for (int y = 0; y < LCD_H; y++) {
        printf("FB %d ", y);
        const uint8_t *row = &s_fb[y * LCD_W];
        for (int x = 0; x < LCD_W;) {
            uint16_t c = s_palette[row[x]];
            int n = 1;
            while (x + n < LCD_W && s_palette[row[x + n]] == c) {
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
