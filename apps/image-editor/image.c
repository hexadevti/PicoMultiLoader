// image.c - view BMP pictures on the SD card and paint on them.
//
// A picture is loaded into the canvas, one byte per pixel, as an index into a 6x8x5 colour cube
// (palette entries 16..255) with ordered dithering. A picture bigger than the canvas is scaled
// down on loading (the title then says "reduced"). The viewer shows the canvas fitted to the
// screen or zoomed; the editor adds a cursor, a few tools and one level of undo. Saving writes
// a 24-bit BMP.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "pico/stdlib.h"
#include "pmlapp.h"

#if PICO_RP2040
#define CANVAS_MAX (48 * 1024)
#else
#define CANVAS_MAX (160 * 1024)
#endif
#define DIM_MAX 2048

// The colour cube: 6 reds, 8 greens, 5 blues.
#define CUBE0 16
#define NR 6
#define NG 8
#define NB 5
#define CUBE(r, g, b) (CUBE0 + ((r) * NG + (g)) * NB + (b))

#define STATUS_H 13
#define VIEW_Y   APP_TOP

static uint8_t s_canvas[CANVAS_MAX];
static uint8_t s_undo[CANVAS_MAX];
static int s_w, s_h, s_undo_w, s_undo_h;
static bool s_has_undo, s_modified, s_reduced;
static char s_path[256];

static const uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

// ---- colours ------------------------------------------------------------------------------------
static inline int level(int i, int n)
{
    return (i * 255 + (n - 1) / 2) / (n - 1);
}

static void index_rgb(uint8_t idx, int *r, int *g, int *b)
{
    int i = idx - CUBE0;
    *b = level(i % NB, NB);
    *g = level(i / NB % NG, NG);
    *r = level(i / (NB * NG), NR);
}

static inline int quant(int v, int n, int t)
{
    // t is the dither threshold 0..15; the step between levels is 255 / (n - 1)
    int q = (v * (n - 1) * 16 + (t - 8) * 255 + 8 * 255) / (255 * 16);
    return q < 0 ? 0 : q >= n ? n - 1 : q;
}

static inline uint8_t dither(int r, int g, int b, int x, int y)
{
    int t = BAYER[y & 3][x & 3];
    return CUBE(quant(r, NR, t), quant(g, NG, t), quant(b, NB, t));
}

static inline uint8_t nearest(int r, int g, int b)
{
    return CUBE((r * (NR - 1) + 127) / 255, (g * (NG - 1) + 127) / 255, (b * (NB - 1) + 127) / 255);
}

static void cube_palette(void)
{
    for (int i = CUBE0; i < 256; i++) {
        int r, g, b;
        index_rgb((uint8_t)i, &r, &g, &b);
        lcd_set_palette((lcd_color_t)i, (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)));
    }
}

// ---- BMP files ----------------------------------------------------------------------------------
static uint32_t rd16(const uint8_t *p)
{
    return p[0] | p[1] << 8;
}

static uint32_t rd32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

typedef struct {
    uint32_t mask;
    int shift, bits;
} field_t;

static field_t field(uint32_t mask)
{
    field_t f = {mask, 0, 0};
    if (mask) {
        while (!(mask & 1)) {
            mask >>= 1;
            f.shift++;
        }
        while (mask & 1) {
            mask >>= 1;
            f.bits++;
        }
    }
    return f;
}

static inline int field_get(field_t f, uint32_t v)
{
    if (!f.bits) {
        return 0;
    }
    uint32_t x = (v & f.mask) >> f.shift;
    return f.bits >= 8 ? (int)(x >> (f.bits - 8)) : (int)(x * 255 / ((1u << f.bits) - 1));
}

static uint8_t s_chunk[4096];

// Load a BMP into the canvas. False (with a message shown) on failure; the canvas is then unchanged.
static bool load_bmp(const char *path)
{
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) {
        app_message("Open", "Can't open the file.");
        return false;
    }
    uint8_t hd[54 + 12];
    UINT n = 0;
    const char *err = NULL;
    if (f_read(&f, hd, sizeof(hd), &n) != FR_OK || n < 54 || hd[0] != 'B' || hd[1] != 'M') {
        err = "Not a BMP file.";
    }
    uint32_t off = rd32(hd + 10), hsize = rd32(hd + 14);
    int32_t w = (int32_t)rd32(hd + 18), h = (int32_t)rd32(hd + 22);
    int bpp = (int)rd16(hd + 28);
    uint32_t comp = rd32(hd + 30), ncol = rd32(hd + 46);
    if (!err && hsize < 40) {
        err = "This old (OS/2) kind of BMP isn't supported.";
    }
    bool top_down = h < 0;
    if (h < 0) {
        h = -h;
    }
    if (!err && (w <= 0 || h <= 0 || w > 32768 || h > 32768)) {
        err = "The picture has a bad size.";
    }
    if (!err && !(comp == 0 || (comp == 3 && (bpp == 16 || bpp == 32)))) {
        err = "Compressed BMP files aren't supported.";
    }
    if (!err && bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) {
        err = "Unsupported number of colours.";
    }
    // the colour table (up to 256 BGRX entries) or the bit fields
    static uint8_t pal[256][3];
    field_t fr = field(0x7C00), fg = field(0x03E0), fb = field(0x001F);
    if (!err && bpp <= 8) {
        uint32_t count = ncol && ncol <= 256 ? ncol : 1u << bpp;
        uint8_t q[4];
        memset(pal, 0, sizeof(pal));
        f_lseek(&f, 14 + hsize);
        for (uint32_t i = 0; i < count; i++) {
            if (f_read(&f, q, 4, &n) != FR_OK || n != 4) {
                break;
            }
            pal[i][0] = q[2];
            pal[i][1] = q[1];
            pal[i][2] = q[0];
        }
    } else if (!err && comp == 3) {
        if (n < 66) {
            err = "Bad BMP header.";
        } else {
            fr = field(rd32(hd + 54));
            fg = field(rd32(hd + 58));
            fb = field(rd32(hd + 62));
        }
    } else if (!err && bpp == 32) {
        fr = field(0x00FF0000);
        fg = field(0x0000FF00);
        fb = field(0x000000FF);
    }
    if (err) {
        f_close(&f);
        app_message("Open", err);
        return false;
    }

    // the canvas size: the picture's, or scaled down to fit
    int tw = w, th = h;
    if ((int64_t)tw * th > CANVAS_MAX || tw > DIM_MAX || th > DIM_MAX) {
        double s = sqrt((double)CANVAS_MAX / ((double)w * h));
        if (w * s > DIM_MAX) {
            s = (double)DIM_MAX / w;
        }
        if (h * s > DIM_MAX) {
            s = (double)DIM_MAX / h;
        }
        tw = (int)(w * s);
        th = (int)(h * s);
        tw = tw < 1 ? 1 : tw;
        th = th < 1 ? 1 : th;
        while ((int64_t)tw * th > CANVAS_MAX) {
            tw > th ? tw-- : th--;
        }
    }

    // Keep the old canvas, to put back if reading fails.
    memcpy(s_undo, s_canvas, (size_t)s_w * s_h);
    s_undo_w = s_w;
    s_undo_h = s_h;

    uint32_t row_bytes = ((uint32_t)w * bpp + 31) / 32 * 4;
    app_toast("Loading...");
    lcd_flush_rows(APP_BOTTOM, APP_FOOTER_H);
    bool ok = true;
    for (int ty = 0; ty < th && ok; ty++) {
        int sy = (int)((int64_t)ty * h / th);
        FSIZE_t row = off + (FSIZE_t)(top_down ? sy : h - 1 - sy) * row_bytes;
        uint32_t c0 = 0, c1 = 0; // the bytes of the row in s_chunk: [c0, c1)
        for (int tx = 0; tx < tw; tx++) {
            int sx = (int)((int64_t)tx * w / tw);
            uint32_t b0 = (uint32_t)sx * bpp / 8, b1 = b0 + (bpp + 7) / 8;
            if (tx == 0 || b0 < c0 || b1 > c1) {
                uint32_t len = row_bytes - b0 < sizeof(s_chunk) ? row_bytes - b0 : sizeof(s_chunk);
                if (f_lseek(&f, row + b0) != FR_OK || f_read(&f, s_chunk, len, &n) != FR_OK || n != len) {
                    ok = false;
                    break;
                }
                c0 = b0;
                c1 = b0 + len;
            }
            const uint8_t *p = &s_chunk[b0 - c0];
            int r, g, b;
            if (bpp <= 8) {
                int i = bpp == 8 ? p[0] : bpp == 4 ? (p[0] >> (sx & 1 ? 0 : 4)) & 15 : (p[0] >> (7 - (sx & 7))) & 1;
                r = pal[i][0];
                g = pal[i][1];
                b = pal[i][2];
            } else if (bpp == 24) {
                b = p[0];
                g = p[1];
                r = p[2];
            } else {
                uint32_t v = bpp == 16 ? rd16(p) : rd32(p);
                r = field_get(fr, v);
                g = field_get(fg, v);
                b = field_get(fb, v);
            }
            s_canvas[ty * tw + tx] = dither(r, g, b, tx, ty);
        }
    }
    f_close(&f);
    if (!ok) {
        memcpy(s_canvas, s_undo, (size_t)s_undo_w * s_undo_h);
        app_message("Open", "Read error.");
        return false;
    }
    s_has_undo = false;
    s_w = tw;
    s_h = th;
    s_reduced = tw != w || th != h;
    s_modified = false;
    strlcpy(s_path, path, sizeof(s_path));
    app_setting_set("last", s_path);
    return true;
}

static bool save_bmp(const char *path)
{
    char tmp[sizeof(s_path) + 4];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FIL f;
    if (f_open(&f, tmp, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        app_message("Save", "Can't create the file. Is the folder there, and the card not locked?");
        return false;
    }
    uint32_t row_bytes = ((uint32_t)s_w * 3 + 3) / 4 * 4, size = 54 + row_bytes * s_h;
    uint8_t hd[54] = {'B', 'M'};
    uint32_t vals[][2] = {{2, size}, {10, 54}, {14, 40}, {18, (uint32_t)s_w}, {22, (uint32_t)s_h},
                          {34, row_bytes * s_h}, {38, 2835}, {42, 2835}};
    for (unsigned i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
        for (int k = 0; k < 4; k++) {
            hd[vals[i][0] + k] = (uint8_t)(vals[i][1] >> (8 * k));
        }
    }
    hd[26] = 1;  // planes
    hd[28] = 24; // bits per pixel
    UINT n;
    bool ok = f_write(&f, hd, 54, &n) == FR_OK && n == 54;
    for (int y = s_h - 1; y >= 0 && ok; y--) {
        // a row fits in s_chunk only up to 1365 pixels: write it in pieces
        uint32_t o = 0;
        for (int x = 0; x < s_w && ok; x++) {
            int r, g, b;
            index_rgb(s_canvas[y * s_w + x], &r, &g, &b);
            s_chunk[o++] = (uint8_t)b;
            s_chunk[o++] = (uint8_t)g;
            s_chunk[o++] = (uint8_t)r;
            if (o + 3 > sizeof(s_chunk)) {
                ok = f_write(&f, s_chunk, o, &n) == FR_OK && n == o;
                o = 0;
            }
        }
        uint32_t pad = row_bytes - (uint32_t)s_w * 3;
        for (uint32_t i = 0; i < pad; i++) {
            s_chunk[o++] = 0;
        }
        if (ok && o) {
            ok = f_write(&f, s_chunk, o, &n) == FR_OK && n == o;
        }
    }
    ok = f_close(&f) == FR_OK && ok;
    if (!ok) {
        f_unlink(tmp);
        app_message("Save", "Write error: the card may be full.");
        return false;
    }
    f_unlink(path);
    if (f_rename(tmp, path) != FR_OK) {
        app_message("Save", "Saved as .tmp: couldn't rename it.");
        return false;
    }
    strlcpy(s_path, path, sizeof(s_path));
    s_modified = s_reduced = false;
    app_setting_set("last", s_path);
    return true;
}

static bool save_as(void)
{
    char path[sizeof(s_path)];
    strlcpy(path, s_path[0] ? s_path : "/picture.bmp", sizeof(path));
    if (!app_input("Save as", "Path of the BMP file on the card:", path, sizeof(path)) || !path[0]) {
        return false;
    }
    if (!app_has_ext(path, ".bmp") && strlen(path) + 4 < sizeof(path)) {
        strcat(path, ".bmp");
    }
    FILINFO fi;
    if (strcmp(path, s_path) != 0 && f_stat(path, &fi) == FR_OK &&
        !app_confirm("Save as", "That file exists. Replace it?")) {
        return false;
    }
    return save_bmp(path);
}

static bool save(void)
{
    if (!s_path[0]) {
        return save_as();
    }
    if (s_reduced && !app_confirm("Save", "The picture was scaled down to fit in memory. Replace the file with the smaller one?")) {
        return save_as();
    }
    return save_bmp(s_path);
}

static bool may_discard(void)
{
    if (!s_modified) {
        return true;
    }
    int a = app_ask("Unsaved changes", "Save the picture first?");
    return a == 0 || (a == 1 && save());
}

static bool open_file(void)
{
    if (!may_discard()) {
        return false;
    }
    char path[sizeof(s_path)];
    FILINFO fi;
    if (s_path[0]) {
        strlcpy(path, s_path, sizeof(path));
    } else {
        strlcpy(path, f_stat("/Pictures", &fi) == FR_OK ? "/Pictures" : "/", sizeof(path));
    }
    return app_pick_file("Open a picture", ".bmp", path, sizeof(path)) && load_bmp(path);
}

// The next (dir > 0) or previous picture in the folder of the current one, by name.
static bool step_file(int dir)
{
    if (!s_path[0] || !may_discard()) {
        return false;
    }
    char folder[sizeof(s_path)], best[sizeof(s_path)] = "";
    app_dirname(s_path, folder, sizeof(folder));
    const char *cur = app_basename(s_path);
    DIR d;
    FILINFO fi;
    if (f_opendir(&d, folder[0] ? folder : "/") != FR_OK) {
        return false;
    }
    char first[sizeof(s_path)] = "";
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        if ((fi.fattrib & (AM_DIR | AM_HID | AM_SYS)) || !app_has_ext(fi.fname, ".bmp")) {
            continue;
        }
        int c = strcasecmp(fi.fname, cur);
        if ((dir > 0 && c > 0 && (!best[0] || strcasecmp(fi.fname, best) < 0)) ||
            (dir < 0 && c < 0 && (!best[0] || strcasecmp(fi.fname, best) > 0))) {
            strlcpy(best, fi.fname, sizeof(best));
        }
        // to wrap around: the first (or last) of all
        if (!first[0] || (dir > 0 ? strcasecmp(fi.fname, first) < 0 : strcasecmp(fi.fname, first) > 0)) {
            strlcpy(first, fi.fname, sizeof(first));
        }
    }
    f_closedir(&d);
    const char *pick = best[0] ? best : first;
    if (!pick[0] || strcasecmp(pick, cur) == 0) {
        return false;
    }
    char path[sizeof(s_path) * 2];
    snprintf(path, sizeof(path), "%s/%s", folder, pick);
    return load_bmp(path);
}

// ---- the view -----------------------------------------------------------------------------------
// The canvas is drawn into a screen rectangle; `step` is canvas pixels per screen pixel in 16.16
// fixed point (65536 / zoom when zoomed in), (ox, oy) the canvas pixel at the top left.
typedef struct {
    int x, y, w, h; // on the screen
    int step, ox, oy;
    int dx, dy;     // where the canvas starts on the screen (centred when it is smaller)
} view_t;

static void view_fix(view_t *v)
{
    int dw = (int)(((int64_t)s_w << 16) / v->step), dh = (int)(((int64_t)s_h << 16) / v->step);
    int vis_w = (int)(((int64_t)v->w * v->step) >> 16), vis_h = (int)(((int64_t)v->h * v->step) >> 16);
    if (dw <= v->w) {
        v->ox = 0;
        v->dx = v->x + (v->w - dw) / 2;
    } else {
        v->ox = v->ox < 0 ? 0 : v->ox > s_w - vis_w ? s_w - vis_w : v->ox;
        v->dx = v->x;
    }
    if (dh <= v->h) {
        v->oy = 0;
        v->dy = v->y + (v->h - dh) / 2;
    } else {
        v->oy = v->oy < 0 ? 0 : v->oy > s_h - vis_h ? s_h - vis_h : v->oy;
        v->dy = v->y;
    }
}

static int fit_step(const view_t *v)
{
    // the biggest whole zoom (up to 8) that fits, or a scale-down
    for (int z = 8; z >= 1; z--) {
        if (s_w * z <= v->w && s_h * z <= v->h) {
            return 65536 / z;
        }
    }
    int a = (int)(((int64_t)s_w << 16) / v->w + 1), b = (int)(((int64_t)s_h << 16) / v->h + 1);
    return a > b ? a : b;
}

static void draw_canvas(const view_t *v)
{
    lcd_fill_rect(v->x, v->y, v->w, v->h, C_BG);
    if (!s_w) {
        return;
    }
    int dw = (int)(((int64_t)s_w << 16) / v->step), dh = (int)(((int64_t)s_h << 16) / v->step);
    int x1 = v->dx + dw < v->x + v->w ? v->dx + dw : v->x + v->w;
    int y1 = v->dy + dh < v->y + v->h ? v->dy + dh : v->y + v->h;
    for (int y = v->dy; y < y1; y++) {
        int cy = v->oy + (int)(((int64_t)(y - v->dy) * v->step) >> 16);
        if (cy >= s_h) {
            break;
        }
        uint8_t *row = app_fb_row(y), *src = &s_canvas[cy * s_w];
        uint32_t acc = (uint32_t)v->ox << 16;
        for (int x = v->dx; x < x1; x++, acc += (uint32_t)v->step) {
            int cx = (int)(acc >> 16);
            if (cx >= s_w) {
                break;
            }
            row[x] = src[cx];
        }
    }
    // a frame around a canvas smaller than the view
    if (dw < v->w || dh < v->h) {
        int fx = v->dx - 1, fy = v->dy - 1, fw = (x1 - v->dx) + 2, fh = (y1 - v->dy) + 2;
        if (fy >= v->y) {
            lcd_hline(fx, fy, fw, C_LINE);
        }
        if (fy + fh - 1 < v->y + v->h) {
            lcd_hline(fx, fy + fh - 1, fw, C_LINE);
        }
        lcd_fill_rect(fx, fy, 1, fh, C_LINE);
        lcd_fill_rect(fx + fw - 1, fy, 1, fh, C_LINE);
    }
}

// Screen position of canvas pixel (cx, cy) when zoomed in (step <= 65536).
static int zoom_of(const view_t *v)
{
    return v->step <= 65536 ? 65536 / v->step : 1;
}

static void screen_of(const view_t *v, int cx, int cy, int *sx, int *sy)
{
    int z = zoom_of(v);
    *sx = v->dx + (cx - v->ox) * z;
    *sy = v->dy + (cy - v->oy) * z;
}

// Paint one canvas pixel on the screen (for previews), clipped to the view.
static void screen_pixel(const view_t *v, int cx, int cy, uint8_t c)
{
    int z = zoom_of(v), sx, sy;
    screen_of(v, cx, cy, &sx, &sy);
    int x0 = sx < v->x ? v->x : sx, y0 = sy < v->y ? v->y : sy;
    int x1 = sx + z > v->x + v->w ? v->x + v->w : sx + z, y1 = sy + z > v->y + v->h ? v->y + v->h : sy + z;
    if (x0 < x1 && y0 < y1) {
        lcd_fill_rect(x0, y0, x1 - x0, y1 - y0, c);
    }
}

// ---- drawing ------------------------------------------------------------------------------------
typedef void (*plot_fn)(int x, int y, uint8_t c);
static const view_t *s_pv; // the view for preview plots

static void plot_canvas(int x, int y, uint8_t c)
{
    if (x >= 0 && y >= 0 && x < s_w && y < s_h) {
        s_canvas[y * s_w + x] = c;
    }
}

static void plot_screen(int x, int y, uint8_t c)
{
    if (x >= 0 && y >= 0 && x < s_w && y < s_h) {
        screen_pixel(s_pv, x, y, c);
    }
}

static void line(int x0, int y0, int x1, int y1, uint8_t c, plot_fn plot)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, e = dx + dy;
    for (;;) {
        plot(x0, y0, c);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * e;
        if (e2 >= dy) {
            e += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            e += dx;
            y0 += sy;
        }
    }
}

static void rect(int x0, int y0, int x1, int y1, bool filled, uint8_t c, plot_fn plot)
{
    if (x0 > x1) {
        int t = x0;
        x0 = x1;
        x1 = t;
    }
    if (y0 > y1) {
        int t = y0;
        y0 = y1;
        y1 = t;
    }
    for (int y = y0; y <= y1; y++) {
        if (filled || y == y0 || y == y1) {
            for (int x = x0; x <= x1; x++) {
                plot(x, y, c);
            }
        } else {
            plot(x0, y, c);
            plot(x1, y, c);
        }
    }
}

static void ellipse(int x0, int y0, int x1, int y1, uint8_t c, plot_fn plot)
{
    // midpoint ellipse in the box (x0, y0)-(x1, y1)
    if (x0 > x1) {
        int t = x0;
        x0 = x1;
        x1 = t;
    }
    if (y0 > y1) {
        int t = y0;
        y0 = y1;
        y1 = t;
    }
    int a = x1 - x0, b = y1 - y0, b1 = b & 1;
    long dx = 4L * (1 - a) * b * b, dy = 4L * (b1 + 1) * a * a, err = dx + dy + (long)b1 * a * a, e2;
    y0 += (b + 1) / 2;
    y1 = y0 - b1;
    long aa = 8L * a * a, bb = 8L * b * b;
    do {
        plot(x1, y0, c);
        plot(x0, y0, c);
        plot(x0, y1, c);
        plot(x1, y1, c);
        e2 = 2 * err;
        if (e2 <= dy) {
            y0++;
            y1--;
            err += dy += aa;
        }
        if (e2 >= dx || 2 * err > dy) {
            x0++;
            x1--;
            err += dx += bb;
        }
    } while (x0 <= x1);
    while (y0 - y1 <= b) {
        plot(x0 - 1, y0, c);
        plot(x1 + 1, y0, c);
        plot(x0 - 1, y1, c);
        plot(x1 + 1, y1, c);
        y0++;
        y1--;
    }
}

// Scanline flood fill of the area of (x, y)'s colour.
static bool flood(int x, int y, uint8_t c)
{
    uint8_t old = s_canvas[y * s_w + x];
    if (old == c) {
        return true;
    }
    static int16_t stack[2048][2];
    int sp = 0;
    bool overflow = false;
    stack[sp][0] = (int16_t)x;
    stack[sp][1] = (int16_t)y;
    sp++;
    while (sp > 0) {
        sp--;
        int px = stack[sp][0], py = stack[sp][1];
        uint8_t *row = &s_canvas[py * s_w];
        if (row[px] != old) {
            continue;
        }
        int l = px, r = px;
        while (l > 0 && row[l - 1] == old) {
            l--;
        }
        while (r < s_w - 1 && row[r + 1] == old) {
            r++;
        }
        memset(&row[l], c, r - l + 1);
        for (int ny = py - 1; ny <= py + 1; ny += 2) {
            if (ny < 0 || ny >= s_h) {
                continue;
            }
            uint8_t *nr = &s_canvas[ny * s_w];
            bool in = false;
            for (int i = l; i <= r; i++) {
                if (nr[i] == old && !in) {
                    if (sp < (int)(sizeof(stack) / sizeof(stack[0]))) {
                        stack[sp][0] = (int16_t)i;
                        stack[sp][1] = (int16_t)ny;
                        sp++;
                    } else {
                        overflow = true;
                    }
                    in = true;
                } else if (nr[i] != old) {
                    in = false;
                }
            }
        }
    }
    return !overflow;
}

static void snapshot(void)
{
    memcpy(s_undo, s_canvas, (size_t)s_w * s_h);
    s_undo_w = s_w;
    s_undo_h = s_h;
    s_has_undo = true;
}

// Undo swaps the canvas with the saved state, so a second undo is a redo.
static void undo(void)
{
    if (!s_has_undo) {
        app_toast("Nothing to undo.");
        return;
    }
    int n = s_w * s_h > s_undo_w * s_undo_h ? s_w * s_h : s_undo_w * s_undo_h;
    for (int i = 0; i < n; i++) {
        uint8_t t = s_canvas[i];
        s_canvas[i] = s_undo[i];
        s_undo[i] = t;
    }
    int tw = s_w, th = s_h;
    s_w = s_undo_w;
    s_h = s_undo_h;
    s_undo_w = tw;
    s_undo_h = th;
    s_modified = true;
}

static bool parse_size(const char *s, int *w, int *h)
{
    char *end;
    long a = strtol(s, &end, 10);
    while (*end == ' ' || *end == 'x' || *end == 'X' || *end == '*' || *end == ',') {
        end++;
    }
    long b = strtol(end, NULL, 10);
    if (a < 1 || b < 1 || a > DIM_MAX || b > DIM_MAX || a * b > CANVAS_MAX) {
        return false;
    }
    *w = (int)a;
    *h = (int)b;
    return true;
}

static bool new_picture(uint8_t bg)
{
    if (!may_discard()) {
        return false;
    }
    char buf[24];
#if PICO_RP2040
    strlcpy(buf, "200x150", sizeof(buf));
#else
    strlcpy(buf, "320x240", sizeof(buf));
#endif
    char prompt[64];
    snprintf(prompt, sizeof(prompt), "Width x height (up to %d pixels in all):", CANVAS_MAX);
    for (;;) {
        if (!app_input("New picture", prompt, buf, sizeof(buf))) {
            return false;
        }
        int w, h;
        if (parse_size(buf, &w, &h)) {
            if (s_w) {
                snapshot();
            }
            s_w = w;
            s_h = h;
            memset(s_canvas, bg, (size_t)w * h);
            s_path[0] = '\0';
            s_modified = s_reduced = false;
            return true;
        }
        app_message("New picture", "Type a size like 160x120, and not too big.");
    }
}

// ---- the colour picker --------------------------------------------------------------------------
// 12 rows of 20: row = red * 2 + green / 4, column = (green % 4) * 5 + blue.
#define PICK_CELL 14
#define PICK_COLS 20
#define PICK_ROWS 12

static uint8_t pick_index(int col, int row)
{
    return CUBE(row / 2, (row % 2) * 4 + col / NB, col % NB);
}

static bool pick_colour(uint8_t *c)
{
    int i = *c - CUBE0, r = i / (NB * NG), g = i / NB % NG, b = i % NB;
    int row = r * 2 + g / 4, col = (g % 4) * NB + b;
    int x0 = (LCD_W - PICK_COLS * PICK_CELL) / 2, y0 = APP_TOP + 22;
    for (;;) {
        app_header("Pick a colour", NULL);
        app_clear_body();
        for (int j = 0; j < PICK_ROWS; j++) {
            for (int k = 0; k < PICK_COLS; k++) {
                lcd_fill_rect(x0 + k * PICK_CELL, y0 + j * PICK_CELL, PICK_CELL - 1, PICK_CELL - 1, pick_index(k, j));
            }
        }
        int sx = x0 + col * PICK_CELL - 2, sy = y0 + row * PICK_CELL - 2;
        look_frame(sx, sy, PICK_CELL + 3, PICK_CELL + 3, C_BRIGHT);
        look_frame(sx + 1, sy + 1, PICK_CELL + 1, PICK_CELL + 1, C_BG);
        uint8_t cur = pick_index(col, row);
        int cr, cg, cb;
        index_rgb(cur, &cr, &cg, &cb);
        int py = y0 + PICK_ROWS * PICK_CELL + 10;
        lcd_fill_rect(x0, py, 40, 24, cur);
        look_frame(x0 - 1, py - 1, 42, 26, C_LINE);
        char t[40];
        snprintf(t, sizeof(t), "#%02X%02X%02X", cr, cg, cb);
        lcd_text(x0 + 50, py + 2, t, C_FG, FONT_LARGE);
        lcd_text(x0 + 50, py + 16, "Enter picks it, # types a hex colour", C_DIM, FONT_SMALL);
        app_footer("Enter Pick  # Hex  Esc Cancel");
        lcd_flush();
        int k = app_wait_key();
        switch (k) {
        case KEY_LEFT:
            col = (col + PICK_COLS - 1) % PICK_COLS;
            break;
        case KEY_RIGHT:
            col = (col + 1) % PICK_COLS;
            break;
        case KEY_UP:
            row = (row + PICK_ROWS - 1) % PICK_ROWS;
            break;
        case KEY_DOWN:
            row = (row + 1) % PICK_ROWS;
            break;
        case KEY_ENTER:
        case ' ':
            *c = cur;
            return true;
        case '#': {
            char hex[12];
            strlcpy(hex, t + 1, sizeof(hex));
            if (app_input("Colour", "Red, green, blue as RRGGBB:", hex, sizeof(hex))) {
                const char *h = hex[0] == '#' ? hex + 1 : hex;
                char *end;
                unsigned long v = strtoul(h, &end, 16);
                if (end - h == 6) {
                    *c = nearest((int)(v >> 16) & 255, (int)(v >> 8) & 255, (int)v & 255);
                    return true;
                }
                app_message("Colour", "Type six hex digits, e.g. FF8000.");
            }
            break;
        }
        case KEY_ESC:
            return false;
        }
    }
}

// ---- the editor ---------------------------------------------------------------------------------
enum { T_PEN, T_LINE, T_RECT, T_FILLED, T_ELLIPSE, T_FILL, T_PICK, T_COUNT };
static const char *const TOOL_NAME[T_COUNT] = {"Pen", "Line", "Rect", "Filled", "Ellipse", "Fill", "Picker"};

static const char *const EDIT_HELP[] = {
    "# Moving",
    "Arrows     Move the cursor",
    "Tab        Move by 1 or by 8 pixels",
    "+ / -      Zoom in / out (1x to 8x)",
    "",
    "# Tools (the letter picks one)",
    "p          Pen: Enter plots, Space puts the",
    "           pen down / up to draw as you move",
    "l          Line: Enter at each end",
    "r / f      Rectangle / filled rectangle",
    "o          Ellipse in a box",
    "b          Bucket fill",
    "i          Picker: Enter takes the colour",
    "",
    "# Colours",
    "c          Pick the colour (# for hex)",
    "x          Swap the colour and the background",
    "",
    "# The picture",
    "u Ctrl+Z   Undo (again: redo)",
    "Ctrl+N     New picture",
    "F2 Ctrl+S  Save; F7 save as",
    "Esc        Back to the viewer",
};

static void edit(void)
{
    static int cx, cy, tool = T_PEN, zoom = 2, step = 1;
    static uint8_t fg = CUBE(0, 0, 0), bg = CUBE(NR - 1, NG - 1, NB - 1);
    bool pen = false, anchor = false;
    int ax = 0, ay = 0;
    view_t v = {0, VIEW_Y, LCD_W, APP_BODY_H - STATUS_H, 65536 / 2, 0, 0, 0, 0};
    if (!s_w && !new_picture(bg)) {
        return;
    }
    cx = cx < s_w ? cx : s_w / 2;
    cy = cy < s_h ? cy : s_h / 2;
    v.ox = cx - LCD_W / (2 * zoom);
    v.oy = cy - v.h / (2 * zoom);
    for (;;) {
        // keep the cursor in view
        v.step = 65536 / zoom;
        int vis_w = v.w / zoom, vis_h = v.h / zoom;
        if (cx < v.ox || cx >= v.ox + vis_w) {
            v.ox = cx - vis_w / 2;
        }
        if (cy < v.oy || cy >= v.oy + vis_h) {
            v.oy = cy - vis_h / 2;
        }
        view_fix(&v);

        char title[64];
        snprintf(title, sizeof(title), "%s%s", s_path[0] ? app_basename(s_path) : "untitled", s_modified ? " *" : "");
        app_header(title, NULL);
        draw_canvas(&v);
        s_pv = &v;
        if (anchor) {
            if (tool == T_LINE) {
                line(ax, ay, cx, cy, fg, plot_screen);
            } else if (tool == T_ELLIPSE) {
                ellipse(ax, ay, cx, cy, fg, plot_screen);
            } else {
                rect(ax, ay, cx, cy, tool == T_FILLED, fg, plot_screen);
            }
        }
        // the cursor: a box around the pixel
        int sx, sy;
        screen_of(&v, cx, cy, &sx, &sy);
        look_frame(sx - 2, sy - 2, zoom + 4, zoom + 4, C_BG);
        look_frame(sx - 1, sy - 1, zoom + 2, zoom + 2, C_BRIGHT);

        int by = APP_BOTTOM - STATUS_H;
        lcd_fill_rect(0, by, LCD_W, STATUS_H, C_PANEL);
        lcd_hline(0, by, LCD_W, C_LINE);
        lcd_fill_rect(3, by + 3, 14, 9, bg);
        lcd_fill_rect(1, by + 2, 10, 7, C_LINE);
        lcd_fill_rect(2, by + 3, 8, 5, fg);
        char st[80];
        snprintf(st, sizeof(st), "%-7s %s %3d,%-3d %dx%d %dx%s", TOOL_NAME[tool],
                 tool == T_PEN ? (pen ? "down" : "up  ") : anchor ? "2nd " : "    ", cx, cy, s_w, s_h, zoom,
                 step > 1 ? " fast" : "");
        lcd_text(22, by + 2, st, C_FG, FONT_SMALL);
        app_footer("F1 Help  c Colour  u Undo  F2 Save  Esc View");
        lcd_flush();

        int k = app_wait_key();
        int ox = cx, oy = cy;
        switch (k) {
        case KEY_LEFT:
            cx = cx - step < 0 ? 0 : cx - step;
            break;
        case KEY_RIGHT:
            cx = cx + step >= s_w ? s_w - 1 : cx + step;
            break;
        case KEY_UP:
            cy = cy - step < 0 ? 0 : cy - step;
            break;
        case KEY_DOWN:
            cy = cy + step >= s_h ? s_h - 1 : cy + step;
            break;
        case KEY_TAB:
            step = step == 1 ? 8 : 1;
            break;
        case '+':
        case '=':
            zoom = zoom < 8 ? zoom * 2 : 8;
            break;
        case '-':
            zoom = zoom > 1 ? zoom / 2 : 1;
            break;
        case 'p':
        case 'l':
        case 'r':
        case 'f':
        case 'o':
        case 'b':
        case 'i':
            tool = strchr("plrfobi", k) - "plrfobi";
            pen = anchor = false;
            break;
        case 'x': {
            uint8_t t = fg;
            fg = bg;
            bg = t;
            break;
        }
        case 'c':
            pick_colour(&fg);
            break;
        case ' ':
            if (tool == T_PEN) {
                pen = !pen;
                if (pen) {
                    snapshot();
                    plot_canvas(cx, cy, fg);
                    s_modified = true;
                }
            }
            break;
        case KEY_ENTER:
            switch (tool) {
            case T_PEN:
                if (!pen) {
                    snapshot();
                }
                plot_canvas(cx, cy, fg);
                s_modified = true;
                break;
            case T_FILL:
                snapshot();
                if (!flood(cx, cy, fg)) {
                    app_toast("The area was too complex to fill in one go: fill again.");
                    sleep_ms(900);
                }
                s_modified = true;
                break;
            case T_PICK:
                fg = s_canvas[cy * s_w + cx];
                tool = T_PEN;
                break;
            default:
                if (!anchor) {
                    anchor = true;
                    ax = cx;
                    ay = cy;
                } else {
                    snapshot();
                    if (tool == T_LINE) {
                        line(ax, ay, cx, cy, fg, plot_canvas);
                    } else if (tool == T_ELLIPSE) {
                        ellipse(ax, ay, cx, cy, fg, plot_canvas);
                    } else {
                        rect(ax, ay, cx, cy, tool == T_FILLED, fg, plot_canvas);
                    }
                    anchor = false;
                    s_modified = true;
                }
                break;
            }
            break;
        case 'u':
        case KEY_CTRL('z'):
            pen = anchor = false;
            undo();
            cx = cx < s_w ? cx : s_w - 1;
            cy = cy < s_h ? cy : s_h - 1;
            break;
        case KEY_CTRL('n'):
            pen = anchor = false;
            if (new_picture(bg)) {
                cx = s_w / 2;
                cy = s_h / 2;
            }
            break;
        case KEY_F2:
        case KEY_CTRL('s'):
            if (save()) {
                app_toast("Saved.");
                lcd_flush_rows(APP_BOTTOM, APP_FOOTER_H);
                sleep_ms(500);
            }
            break;
        case KEY_F7:
            save_as();
            break;
        case KEY_F1:
            app_help("Image Editor help", EDIT_HELP, sizeof(EDIT_HELP) / sizeof(EDIT_HELP[0]));
            break;
        case KEY_ESC:
            if (anchor || pen) {
                pen = anchor = false;
                break;
            }
            return;
        }
        if (pen && (cx != ox || cy != oy)) {
            line(ox, oy, cx, cy, fg, plot_canvas);
            s_modified = true;
        }
    }
}

// ---- the viewer ---------------------------------------------------------------------------------
static const char *const VIEW_HELP[] = {
    "# Viewing",
    "PgUp/PgDn  Previous / next picture in the folder",
    "Left/Right The same, when the picture fits",
    "+ / -      Zoom in / out; 0 fits it again",
    "Arrows     Move around a zoomed picture",
    "",
    "# Files",
    "E Enter    Edit the picture",
    "F3 Ctrl+O  Open a picture",
    "Ctrl+N     New picture",
    "F2         Save; F7 save as",
    "Esc        Quit",
    "",
    "Pictures are BMP files (1, 4, 8, 16, 24 or",
    "32 bits). Big ones are scaled down to fit",
    "in memory, and shown with 240 colours.",
};

int main(void)
{
    app_init();
    cube_palette();
    char last[sizeof(s_path)];
    FILINFO fi;
    if (app_setting_get("last", last, sizeof(last)) && f_stat(last, &fi) == FR_OK) {
        load_bmp(last);
    } else {
        open_file();
    }
    view_t v = {0, VIEW_Y, LCD_W, APP_BODY_H, 65536, 0, 0, 0, 0};
    int zoom = 0; // 0: fit
    for (;;) {
        v.step = zoom ? 65536 / zoom : (s_w ? fit_step(&v) : 65536);
        view_fix(&v);
        char title[64], right[32] = "";
        snprintf(title, sizeof(title), "%s%s", s_path[0] ? app_basename(s_path) : s_w ? "untitled" : APP_NAME,
                 s_modified ? " *" : "");
        if (s_w) {
            snprintf(right, sizeof(right), "%dx%d%s", s_w, s_h, s_reduced ? " reduced" : "");
        }
        app_header(title, right);
        draw_canvas(&v);
        if (!s_w) {
            lcd_text(60, APP_TOP + 120, "No picture open.", C_FG, FONT_LARGE);
            lcd_text(60, APP_TOP + 140, "F3 opens one, Ctrl+N makes one.", C_DIM, FONT_SMALL);
        }
        app_footer("F1 Help  E Edit  F3 Open  PgDn Next  Esc Quit");
        lcd_flush();

        int k = app_wait_key();
        int pan = zoom ? 32 / zoom + 1 : 0;
        switch (k) {
        case KEY_LEFT:
            if (zoom && (s_w << 16) / v.step > v.w) {
                v.ox -= pan;
            } else if (step_file(-1)) {
                zoom = 0;
            }
            break;
        case KEY_RIGHT:
            if (zoom && (s_w << 16) / v.step > v.w) {
                v.ox += pan;
            } else if (step_file(1)) {
                zoom = 0;
            }
            break;
        case KEY_UP:
            v.oy -= pan;
            break;
        case KEY_DOWN:
            v.oy += pan;
            break;
        case KEY_PAGE_UP:
            if (step_file(-1)) {
                zoom = 0;
            }
            break;
        case KEY_PAGE_DOWN:
            if (step_file(1)) {
                zoom = 0;
            }
            break;
        case '+':
        case '=': {
            int now = zoom ? zoom : 65536 / v.step; // 0 for a scaled-down fit: go to 1:1
            int z = now < 1 ? 1 : now * 2 > 8 ? 8 : now * 2;
            // keep the centre where it is
            int cxm = v.ox + (int)(((int64_t)v.w / 2 * v.step) >> 16), cym = v.oy + (int)(((int64_t)v.h / 2 * v.step) >> 16);
            zoom = z;
            v.ox = cxm - v.w / (2 * z);
            v.oy = cym - v.h / (2 * z);
            break;
        }
        case '-':
            zoom = zoom > 1 ? zoom / 2 : 0;
            break;
        case '0':
            zoom = 0;
            break;
        case 'e':
        case 'E':
        case KEY_ENTER:
            edit();
            break;
        case KEY_CTRL('n'):
            if (new_picture(CUBE(NR - 1, NG - 1, NB - 1))) {
                edit();
            }
            break;
        case KEY_F3:
        case KEY_CTRL('o'):
            if (open_file()) {
                zoom = 0;
            }
            break;
        case KEY_F2:
            if (s_w) {
                save();
            }
            break;
        case KEY_F7:
            if (s_w) {
                save_as();
            }
            break;
        case KEY_F1:
            app_help("Image Viewer help", VIEW_HELP, sizeof(VIEW_HELP) / sizeof(VIEW_HELP[0]));
            break;
        case KEY_ESC:
            if (may_discard()) {
                app_exit(NULL);
            }
            break;
        }
    }
}
