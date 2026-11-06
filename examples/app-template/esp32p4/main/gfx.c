// gfx.c - see gfx.h
#include "gfx.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "font.h"

static uint16_t *s_fb;
static int s_w, s_h;
static int s_cx0, s_cy0, s_cx1, s_cy1; // clip rectangle, x1/y1 exclusive

void gfx_init(uint16_t *fb, int w, int h)
{
    s_fb = fb;
    s_w = w;
    s_h = h;
    gfx_noclip();
}

void gfx_clip(int x, int y, int w, int h)
{
    s_cx0 = x < 0 ? 0 : x;
    s_cy0 = y < 0 ? 0 : y;
    s_cx1 = x + w > s_w ? s_w : x + w;
    s_cy1 = y + h > s_h ? s_h : y + h;
}

void gfx_noclip(void)
{
    gfx_clip(0, 0, s_w, s_h);
}

void gfx_clear(uint16_t c)
{
    for (int i = 0; i < s_w * s_h; i++) {
        s_fb[i] = c;
    }
}

void gfx_pixel(int x, int y, uint16_t c)
{
    if (x >= s_cx0 && x < s_cx1 && y >= s_cy0 && y < s_cy1) {
        s_fb[y * s_w + x] = c;
    }
}

void gfx_fill_rect(int x, int y, int w, int h, uint16_t c)
{
    int x0 = x < s_cx0 ? s_cx0 : x;
    int y0 = y < s_cy0 ? s_cy0 : y;
    int x1 = x + w > s_cx1 ? s_cx1 : x + w;
    int y1 = y + h > s_cy1 ? s_cy1 : y + h;
    for (int yy = y0; yy < y1; yy++) {
        uint16_t *p = &s_fb[yy * s_w];
        for (int xx = x0; xx < x1; xx++) {
            p[xx] = c;
        }
    }
}

void gfx_hline(int x, int y, int w, uint16_t c)
{
    gfx_fill_rect(x, y, w, 1, c);
}

void gfx_vline(int x, int y, int h, uint16_t c)
{
    gfx_fill_rect(x, y, 1, h, c);
}

void gfx_line(int x0, int y0, int x1, int y1, uint16_t c)
{
    // Bresenham
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        gfx_pixel(x0, y0, c);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void gfx_rect(int x, int y, int w, int h, uint16_t c)
{
    gfx_hline(x, y, w, c);
    gfx_hline(x, y + h - 1, w, c);
    gfx_vline(x, y, h, c);
    gfx_vline(x + w - 1, y, h, c);
}

void gfx_circle(int cx, int cy, int r, uint16_t c)
{
    // Midpoint circle: one octant, mirrored eight ways
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        gfx_pixel(cx + x, cy + y, c);
        gfx_pixel(cx - x, cy + y, c);
        gfx_pixel(cx + x, cy - y, c);
        gfx_pixel(cx - x, cy - y, c);
        gfx_pixel(cx + y, cy + x, c);
        gfx_pixel(cx - y, cy + x, c);
        gfx_pixel(cx + y, cy - x, c);
        gfx_pixel(cx - y, cy - x, c);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

void gfx_fill_circle(int cx, int cy, int r, uint16_t c)
{
    for (int dy = -r; dy <= r; dy++) {
        int dx = 0;
        while ((dx + 1) * (dx + 1) + dy * dy <= r * r) {
            dx++;
        }
        gfx_hline(cx - dx, cy + dy, 2 * dx + 1, c);
    }
}

void gfx_fill_round_rect(int x, int y, int w, int h, int r, uint16_t c)
{
    if (2 * r > w) {
        r = w / 2;
    }
    if (2 * r > h) {
        r = h / 2;
    }
    gfx_fill_rect(x, y + r, w, h - 2 * r, c);
    for (int dy = 0; dy < r; dy++) {
        // the width of the corner arc on this row
        int yy = r - dy;
        int dx = 0;
        while ((dx + 1) * (dx + 1) + yy * yy <= r * r) {
            dx++;
        }
        int inset = r - dx;
        gfx_hline(x + inset, y + dy, w - 2 * inset, c);
        gfx_hline(x + inset, y + h - 1 - dy, w - 2 * inset, c);
    }
}

void gfx_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c)
{
    // Sort by y, then fill one row at a time between the long edge (0-2) and the short ones.
    int t;
#define SWAP(a, b) (t = a, a = b, b = t)
    if (y0 > y1) {
        SWAP(y0, y1);
        SWAP(x0, x1);
    }
    if (y1 > y2) {
        SWAP(y1, y2);
        SWAP(x1, x2);
    }
    if (y0 > y1) {
        SWAP(y0, y1);
        SWAP(x0, x1);
    }
#undef SWAP
    if (y0 == y2) {
        int lo = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
        int hi = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
        gfx_hline(lo, y0, hi - lo + 1, c);
        return;
    }
    for (int y = y0; y <= y2; y++) {
        int xa = x0 + (x2 - x0) * (y - y0) / (y2 - y0);
        int xb;
        if (y < y1) {
            xb = x0 + (x1 - x0) * (y - y0) / (y1 - y0);
        } else if (y2 != y1) {
            xb = x1 + (x2 - x1) * (y - y1) / (y2 - y1);
        } else {
            xb = x1;
        }
        if (xa > xb) {
            t = xa;
            xa = xb;
            xb = t;
        }
        gfx_hline(xa, y, xb - xa + 1, c);
    }
}

uint16_t gfx_blend(uint16_t a, uint16_t b, int alpha)
{
    int r = ((a >> 11) * (255 - alpha) + (b >> 11) * alpha) / 255;
    int g = (((a >> 5) & 0x3F) * (255 - alpha) + ((b >> 5) & 0x3F) * alpha) / 255;
    int bl = ((a & 0x1F) * (255 - alpha) + (b & 0x1F) * alpha) / 255;
    return (uint16_t)(r << 11 | g << 5 | bl);
}

// ---- text --------------------------------------------------------------------------------

static void font_info(gfx_font_t font, const uint8_t **glyphs, int *w, int *h)
{
    if (font == FONT_LARGE) {
        *glyphs = UIFONT_7X13;
        *w = 7;
        *h = 13;
    } else {
        *glyphs = UIFONT_6X10;
        *w = 6;
        *h = 10;
    }
}

int gfx_font_height(gfx_font_t font, int scale)
{
    return (font == FONT_LARGE ? 13 : 10) * scale;
}

int gfx_text_width(const char *s, gfx_font_t font, int scale)
{
    int cw = font == FONT_LARGE ? 7 : 6;
    int n = 0, best = 0;
    for (; *s; s++) {
        n = *s == '\n' ? 0 : n + 1;
        best = n > best ? n : best;
    }
    return best * cw * scale;
}

int gfx_text(int x, int y, const char *s, uint16_t c, gfx_font_t font, int scale)
{
    const uint8_t *glyphs;
    int cw, ch;
    font_info(font, &glyphs, &cw, &ch);
    int x0 = x;
    for (; *s; s++) {
        unsigned char k = (unsigned char)*s;
        if (k == '\n') {
            x = x0;
            y += ch * scale;
            continue;
        }
        if (k < 0x20 || k > 0x7E) {
            k = '?';
        }
        const uint8_t *g = &glyphs[(k - 0x20) * ch];
        for (int row = 0; row < ch; row++) {
            uint8_t bits = g[row];
            for (int col = 0; bits; col++, bits <<= 1) {
                if (bits & 0x80) {
                    if (scale == 1) {
                        gfx_pixel(x + col, y + row, c);
                    } else {
                        gfx_fill_rect(x + col * scale, y + row * scale, scale, scale, c);
                    }
                }
            }
        }
        x += cw * scale;
    }
    return x;
}

int gfx_printf(int x, int y, uint16_t c, gfx_font_t font, int scale, const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return gfx_text(x, y, buf, c, font, scale);
}
