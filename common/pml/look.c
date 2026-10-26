#include "look.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "plat.h"
#include "sdcard.h"

#define HEX565(v)   RGB565(((v) >> 16) & 0xFF, ((v) >> 8) & 0xFF, (v) & 0xFF)
#define THEME_COLS  13 // entries 0..12; the rest are cycled
#define CYCLE_US    120000
#define THEME_NAME_MAX    24

typedef struct {
    const char *name;
    uint16_t c[THEME_COLS]; // in palette-role order (look.h)
} theme_t;

static const theme_t THEMES[] = {
    {"Graphite", {HEX565(0x0e131c), HEX565(0x172030), HEX565(0x212c40), HEX565(0x33415c), HEX565(0x7a88a3),
                  HEX565(0xd5ddeb), HEX565(0xffffff), HEX565(0x4cc3ff), HEX565(0x1d5f86), HEX565(0x5fd98a),
                  HEX565(0xffb347), HEX565(0xff6b6b), HEX565(0xb48cff)}},
    {"Phosphor", {HEX565(0x04110a), HEX565(0x0a1f12), HEX565(0x10301c), HEX565(0x1d4a2c), HEX565(0x3f8a55),
                  HEX565(0x8cf0a5), HEX565(0xd8ffe2), HEX565(0x5cff8a), HEX565(0x1f7a3c), HEX565(0x5cff8a),
                  HEX565(0xe6ff5c), HEX565(0xff7a5c), HEX565(0xa0ffd0)}},
    {"Amber", {HEX565(0x140c02), HEX565(0x24170a), HEX565(0x352310), HEX565(0x523718), HEX565(0xa0723a),
               HEX565(0xffc670), HEX565(0xfff0d0), HEX565(0xffa020), HEX565(0x8a5208), HEX565(0xd0e060),
               HEX565(0xffd040), HEX565(0xff5a3a), HEX565(0xffd8a0)}},
    {"Paper", {HEX565(0xe9e6dc), HEX565(0xf6f4ee), HEX565(0xdcd8cb), HEX565(0xb9b3a2), HEX565(0x6f6a5d),
               HEX565(0x24221d), HEX565(0x000000), HEX565(0xc2410c), HEX565(0xf0c8a8), HEX565(0x2f7d3a),
               HEX565(0xb8740a), HEX565(0xb42318), HEX565(0x5b4bb7)}},
    {"Clockwork", {HEX565(0x1b1d21), HEX565(0x26292e), HEX565(0x31353b), HEX565(0x474c55), HEX565(0x8b929e),
                   HEX565(0xe3e6ea), HEX565(0xffffff), HEX565(0xff8a3d), HEX565(0x7a3b12), HEX565(0x7bd88f),
                   HEX565(0xffc24d), HEX565(0xff6161), HEX565(0x7cc7ff)}},
};
#define NTHEMES ((int)(sizeof(THEMES) / sizeof(THEMES[0])))

static char s_name[THEME_NAME_MAX];
static uint16_t s_ramp[3]; // accent, between, dark accent
static int s_phase;
static uint16_t s_pal[THEME_COLS]; // the loaded theme
static int s_y0 = LCD_H, s_y1; // rows s_y0..s_y1-1 use the cycled colours
static uint64_t s_next_us;

// ---- themes -----------------------------------------------------------------------------------
static int builtin_index(const char *name)
{
    for (int i = 0; i < NTHEMES; i++) {
        if (strcasecmp(name, THEMES[i].name) == 0) {
            return i;
        }
    }
    return -1;
}

// A .pal file: RGB565 values ("0x1d5f") or #rrggbb colours, in palette-role order, separated by
// anything; ";" starts a comment. Needs all 13; more are ignored.
static bool read_pal(const char *name, uint16_t *out)
{
    char path[sizeof(THEME_DIR) + THEME_NAME_MAX + 6], buf[384];
    FIL f;
    UINT n;
    snprintf(path, sizeof(path), THEME_DIR "/%s.pal", name);
    if (f_open(&f, path, FA_READ) != FR_OK) {
        return false;
    }
    bool ok = f_read(&f, buf, sizeof(buf) - 1, &n) == FR_OK;
    f_close(&f);
    if (!ok) {
        return false;
    }
    buf[n] = '\0';

    int count = 0;
    for (const char *s = buf; *s && count < THEME_COLS;) {
        if (*s == ';') {
            while (*s && *s != '\n') {
                s++;
            }
        } else if (*s == '#' || (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))) {
            bool rgb = *s == '#';
            s += rgb ? 1 : 2;
            char *end;
            unsigned long v = strtoul(s, &end, 16);
            if (end - s != (rgb ? 6 : 4)) {
                return false;
            }
            out[count++] = rgb ? HEX565(v) : (uint16_t)v;
            s = end;
        } else {
            s++;
        }
    }
    return count == THEME_COLS;
}

bool theme_load(const char *name)
{
    uint16_t pal[THEME_COLS];
    int i = name ? builtin_index(name) : -1;
    bool ok = true;
    if (name && strlen(name) < THEME_NAME_MAX && read_pal(name, pal)) {
        strlcpy(s_name, i >= 0 ? THEMES[i].name : name, sizeof(s_name));
    } else {
        ok = i >= 0;
        if (!ok) {
            i = 0;
        }
        memcpy(pal, THEMES[i].c, sizeof(pal));
        strlcpy(s_name, THEMES[i].name, sizeof(s_name));
    }
    memcpy(s_pal, pal, sizeof(s_pal));
    theme_apply();
    return ok;
}

void theme_apply(void)
{
    const uint16_t *pal = s_pal;
    for (int k = 0; k < THEME_COLS; k++) {
        lcd_set_palette(k, pal[k]);
    }
    s_ramp[0] = pal[C_ACCENT];
    s_ramp[1] = (uint16_t)(((pal[C_ACCENT] & 0xF7DE) + (pal[C_ACCDK] & 0xF7DE)) >> 1); // per-channel average
    s_ramp[2] = pal[C_ACCDK];
    for (int k = 0; k < 3; k++) {
        lcd_set_palette(C_CYC0 + k, s_ramp[(k + s_phase) % 3]);
    }
}

const char *theme_name(void)
{
    return s_name;
}

void theme_next(char *out, size_t len)
{
    int i = builtin_index(s_name);
    if (i >= 0 && i + 1 < NTHEMES) {
        strlcpy(out, THEMES[i + 1].name, len);
        return;
    }
    // The .pal files in folder order: the first one after the last built-in theme, else the one
    // after the loaded file.
    bool take = i >= 0;
    FF_DIR d;
    FILINFO fi;
    if (f_opendir(&d, THEME_DIR) == FR_OK) {
        while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
            size_t n = strlen(fi.fname);
            if ((fi.fattrib & AM_DIR) || n < 5 || n - 4 >= THEME_NAME_MAX || strcasecmp(fi.fname + n - 4, ".pal") != 0) {
                continue;
            }
            fi.fname[n - 4] = '\0';
            if (builtin_index(fi.fname) >= 0) {
                continue; // overrides a built-in theme, which is already in the list
            }
            if (take) {
                strlcpy(out, fi.fname, len);
                f_closedir(&d);
                return;
            }
            take = strcasecmp(fi.fname, s_name) == 0;
        }
        f_closedir(&d);
    }
    strlcpy(out, THEMES[0].name, len);
}

// ---- effects ----------------------------------------------------------------------------------
void look_anim_reset(void)
{
    s_y0 = LCD_H;
    s_y1 = 0;
}

static void anim_rows(int y, int h)
{
    if (y < s_y0) {
        s_y0 = y < 0 ? 0 : y;
    }
    if (y + h > s_y1) {
        s_y1 = y + h > LCD_H ? LCD_H : y + h;
    }
}

static inline lcd_color_t cyc(int k)
{
    return (lcd_color_t)(C_CYC0 + (k >> 2) % 3);
}

// One row of 4-pixel runs; pixel i gets cyc(i + off).
static void cyc_row(int x, int y, int w, int off)
{
    for (int i = x; i < x + w;) {
        int run = 4 - ((i + off) & 3);
        if (i + run > x + w) {
            run = x + w - i;
        }
        lcd_fill_rect(i, y, run, 1, cyc(i + off));
        i += run;
    }
}

void look_frame(int x, int y, int w, int h, lcd_color_t color)
{
    lcd_fill_rect(x, y, w, 1, color);
    lcd_fill_rect(x, y + h - 1, w, 1, color);
    lcd_fill_rect(x, y, 1, h, color);
    lcd_fill_rect(x + w - 1, y, 1, h, color);
}

void look_march(int x, int y, int w, int h)
{
    cyc_row(x, y, w, y);
    cyc_row(x, y + h - 1, w, y + h);
    for (int j = y + 1; j < y + h - 1; j++) {
        lcd_fill_rect(x, j, 1, 1, cyc(x + j));
        lcd_fill_rect(x + w - 1, j, 1, 1, cyc(x + w + j));
    }
    anim_rows(y, h);
}

void look_stripes(int x, int y, int w, int h)
{
    for (int j = y; j < y + h; j++) {
        cyc_row(x, j, w, j);
    }
    anim_rows(y, h);
}

void look_dither(int y, int h)
{
    lcd_checker(y, h, C_BG);
}

void look_step(void)
{
    s_phase = (s_phase + 2) % 3; // the pattern moves right and down
    for (int k = 0; k < 3; k++) {
        lcd_set_palette(C_CYC0 + k, s_ramp[(k + s_phase) % 3]);
    }
}

void look_tick(void)
{
    uint64_t now = time_us_64();
    if (s_y1 <= s_y0 || now < s_next_us) {
        return;
    }
    s_next_us = now + CYCLE_US;
    look_step();
    lcd_flush_rows(s_y0, s_y1 - s_y0);
}
