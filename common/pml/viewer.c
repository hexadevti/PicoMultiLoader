// viewer.c - the text reader, the BMP viewer and screenshots (see viewer.h)
#include "viewer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "apps.h"
#include "lcd.h"
#include "look.h"
#include "sdcard.h"
#include "uf2_image.h"

static bool fail(char *err, size_t len, const char *msg)
{
    snprintf(err, len, "%s", msg);
    return false;
}

static bool ends_with(const char *name, const char *ext)
{
    size_t n = strlen(name), e = strlen(ext);
    return n > e && strcasecmp(name + n - e, ext) == 0;
}

view_kind_t view_kind(const char *name)
{
    if (ends_with(name, ".txt") || ends_with(name, ".log") || ends_with(name, ".ini") || ends_with(name, ".bas") ||
        ends_with(name, ".py") || ends_with(name, ".csv") || ends_with(name, ".pal")) {
        return VIEW_TEXT;
    }
    if (ends_with(name, ".md")) {
        return VIEW_MARKDOWN;
    }
    if (ends_with(name, ".bmp")) {
        return VIEW_IMAGE;
    }
    return VIEW_NONE;
}

// ---- text reader ------------------------------------------------------------------------------
// The file stays on the card: each page is laid out from its start offset, read into the shared
// 2 KB scratch buffer. The start of every page seen so far is kept, so going back is cheap.
#define TEXT_MAX_PAGES 8192
#define TEXT_MAX_COLS  64

typedef struct {
    FIL f;
    uint32_t size;
    uint32_t *page_at; // start offset of each page known so far
    int known, cap;    // pages in page_at; its size
    int page;
    int last;          // the last page, -1 = not reached yet
    int cols, rows;
    bool markdown;
} text_t;

static text_t *s_text;

bool text_open(const char *path, bool markdown, int cols, int rows, char *err, size_t len)
{
    text_close();
    text_t *t = calloc(1, sizeof(*t));
    if (!t || !(t->page_at = malloc(64 * sizeof(uint32_t)))) {
        free(t);
        return fail(err, len, "Not enough memory");
    }
    if (f_open(&t->f, path, FA_READ) != FR_OK) {
        free(t->page_at);
        free(t);
        return fail(err, len, "Cannot open the file");
    }
    t->size = (uint32_t)f_size(&t->f);
    t->cap = 64;
    t->known = 1;
    t->page_at[0] = 0;
    t->last = -1;
    t->cols = cols < TEXT_MAX_COLS ? cols : TEXT_MAX_COLS;
    t->rows = rows;
    t->markdown = markdown;
    s_text = t;
    return true;
}

void text_close(void)
{
    if (s_text) {
        f_close(&s_text->f);
        free(s_text->page_at);
        free(s_text);
        s_text = NULL;
    }
}

// One row of text from buf[pos..n): at most `cols` characters, broken at a space if the line is
// longer. Sets *line_end if the row ended its source line. Returns where the next row starts.
static size_t text_row(const uint8_t *buf, size_t pos, size_t n, int cols, char *out, bool *line_end)
{
    int col = 0, brk_col = 0;
    size_t p = pos, brk_p = 0;
    *line_end = false;
    while (p < n && col < cols) {
        uint8_t c = buf[p++];
        if (c == '\n') {
            *line_end = true;
            break;
        }
        if (c == '\t') {
            do {
                out[col++] = ' ';
            } while (col < cols && (col & 3));
            brk_p = p;
            brk_col = col;
        } else if (c >= 0x80) { // UTF-8: one '?' per character
            while (p < n && (buf[p] & 0xC0) == 0x80) {
                p++;
            }
            out[col++] = '?';
        } else if (c >= 0x20) {
            out[col++] = (char)c;
            if (c == ' ') {
                brk_p = p;
                brk_col = col;
            }
        }
    }
    if (!*line_end && col == cols && p < n) { // full: wrap
        if (buf[p] == ' ' || buf[p] == '\r' || buf[p] == '\n') {
            while (p < n && buf[p] == ' ') {
                p++;
            }
            if (p < n && buf[p] == '\r') {
                p++;
            }
            if (p < n && buf[p] == '\n') {
                p++;
                *line_end = true;
            }
        } else if (brk_p > pos) {
            p = brk_p;
            col = brk_col;
        }
    }
    while (col > 0 && out[col - 1] == ' ') {
        col--;
    }
    out[col] = '\0';
    return p;
}

// Lay out the page starting at `at`, drawing it if `draw`. Returns where the next page starts
// (t->size at the end of the file).
static uint32_t text_layout(text_t *t, uint32_t at, bool draw, int x, int y, int line_h)
{
    size_t cap;
    uint8_t *buf = uf2_scratch(&cap);
    // one byte before the page, to know if it starts a source line
    uint32_t from = at > 0 ? at - 1 : 0;
    UINT n = 0;
    if (f_lseek(&t->f, from) != FR_OK || f_read(&t->f, buf, (UINT)cap, &n) != FR_OK) {
        return t->size;
    }
    bool eof = from + n >= t->size;
    size_t pos = at - from;
    bool line_start = at == 0 || buf[0] == '\n';
    bool heading = false;
    char row[TEXT_MAX_COLS + 1];
    for (int r = 0; r < t->rows && pos < n; r++) {
        if (!eof && n - pos < (size_t)t->cols * 4 + 2) {
            break; // the rest of the buffer may not hold a whole row; the next page starts here
        }
        size_t start = pos;
        lcd_color_t color = C_FG;
        if (t->markdown && line_start) {
            heading = buf[pos] == '#';
            while (heading && pos < n && buf[pos] == '#') {
                pos++;
            }
            if (heading && pos < n && buf[pos] == ' ') {
                pos++;
            }
        }
        bool line_end;
        pos = text_row(buf, pos, n, t->cols, row, &line_end);
        if (t->markdown) {
            if (heading) {
                color = C_ACCENT;
            } else if (row[0] == '>' || strncmp(row, "```", 3) == 0 || strncmp(row, "    ", 4) == 0) {
                color = C_DIM;
            }
        }
        if (draw) {
            if (t->markdown && line_start && line_end && strncmp(row, "---", 3) == 0 &&
                strspn(row, "-") == strlen(row)) {
                lcd_hline(x, y + r * line_h + line_h / 2 - 1, t->cols * 6, C_LINE);
            } else {
                lcd_text(x, y + r * line_h, row, color, FONT_SMALL);
            }
        }
        if (line_end) {
            heading = false;
        }
        line_start = line_end;
        if (pos == start) {
            break; // nothing consumed: can't happen, but never loop forever
        }
    }
    uint32_t next = from + (uint32_t)pos;
    return pos >= n && eof ? t->size : next;
}

// Note where the page after the last known one starts (`next`, from laying that one out)
static bool text_add(text_t *t, uint32_t next)
{
    if (next >= t->size || t->known >= TEXT_MAX_PAGES) {
        t->last = t->known - 1;
        return false;
    }
    if (t->known == t->cap) {
        uint32_t *more = realloc(t->page_at, 2 * t->cap * sizeof(uint32_t));
        if (!more) {
            t->last = t->known - 1;
            return false;
        }
        t->page_at = more;
        t->cap *= 2;
    }
    t->page_at[t->known++] = next;
    return true;
}

static bool text_learn(text_t *t)
{
    return text_add(t, text_layout(t, t->page_at[t->known - 1], false, 0, 0, 0));
}

void text_draw(int x, int y, int line_h)
{
    text_t *t = s_text;
    if (!t) {
        return;
    }
    uint32_t next = text_layout(t, t->page_at[t->page], true, x, y, line_h);
    if (t->page == t->known - 1 && t->last < 0) {
        text_add(t, next);
    }
}

bool text_go(int page)
{
    text_t *t = s_text;
    if (!t || page < 0) {
        return false;
    }
    while (page >= t->known && t->last < 0) {
        text_learn(t);
    }
    if (page >= t->known) {
        return false;
    }
    t->page = page;
    return true;
}

void text_end(void)
{
    if (s_text) {
        text_go(TEXT_MAX_PAGES);
        s_text->page = s_text->known - 1;
    }
}

int text_page(void)
{
    return s_text ? s_text->page : 0;
}

bool text_at_end(void)
{
    return !s_text || s_text->page == s_text->last;
}

int text_percent(void)
{
    text_t *t = s_text;
    if (!t || t->size == 0 || text_at_end()) {
        return 100;
    }
    uint32_t end = t->page + 1 < t->known ? t->page_at[t->page + 1] : t->page_at[t->page];
    return (int)((uint64_t)end * 100 / t->size);
}

// ---- BMP viewer -------------------------------------------------------------------------------
// Uncompressed 1/4/8/16/24/32-bit BMPs. Up to 16 colours: shown with their own palette. Others:
// ordered (Bayer) dithering to a fixed palette of 2 reds x 4 greens x 2 blues, or 16 greys.
static const uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

typedef struct {
    FIL f;
    uint8_t *buf;
    size_t cap;
    uint32_t at;  // file offset of buf[0]
    UINT len;
} reader_t;

// `need` bytes at file offset `off`, or NULL
static const uint8_t *bytes_at(reader_t *r, uint32_t off, UINT need)
{
    if (off < r->at || off + need > r->at + r->len) {
        if (f_lseek(&r->f, off) != FR_OK || f_read(&r->f, r->buf, (UINT)r->cap, &r->len) != FR_OK) {
            r->len = 0;
        }
        r->at = off;
        if (r->len < need) {
            return NULL;
        }
    }
    return r->buf + (off - r->at);
}

static uint32_t le16(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8;
}

static uint32_t le32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// The lowest bit set in `mask` and how many bits wide it is
static void mask_shift(uint32_t mask, int *shift, int *bits)
{
    *shift = *bits = 0;
    if (!mask) {
        return;
    }
    while (!(mask & 1)) {
        mask >>= 1;
        (*shift)++;
    }
    while (mask & 1) {
        mask >>= 1;
        (*bits)++;
    }
}

static uint8_t channel(uint32_t px, uint32_t mask, int shift, int bits)
{
    if (!bits) {
        return 0;
    }
    uint32_t v = (px & mask) >> shift;
    return (uint8_t)(bits >= 8 ? v >> (bits - 8) : v * 255 / ((1u << bits) - 1));
}

// level 0..levels-1 for an 8-bit value, dithered by threshold t (0..15)
static int dither(int v, int levels, int t)
{
    int q = (v * (levels - 1) * 32 + (2 * t + 1) * 255) / (255 * 32);
    return q < levels ? q : levels - 1;
}

bool image_show(const char *path, bool grey, char *err, size_t len)
{
    reader_t *r = calloc(1, sizeof(*r));
    uint32_t *pal = NULL;
    bool ok = false;
    if (!r) {
        return fail(err, len, "Not enough memory");
    }
    r->buf = uf2_scratch(&r->cap);
    if (f_open(&r->f, path, FA_READ) != FR_OK) {
        free(r);
        return fail(err, len, "Cannot open the file");
    }
    const uint8_t *h = bytes_at(r, 0, 54);
    if (!h || h[0] != 'B' || h[1] != 'M') {
        fail(err, len, "Not a BMP file");
        goto done;
    }
    uint32_t data = le32(h + 10), dib = le32(h + 14), comp = le32(h + 30), used = le32(h + 46);
    int32_t w = (int32_t)le32(h + 18), hh = (int32_t)le32(h + 22);
    int bpp = (int)le16(h + 28);
    bool top_down = hh < 0;
    if (top_down) {
        hh = -hh;
    }
    if (dib < 40 || w <= 0 || hh <= 0 || w > 16384 || hh > 16384) {
        fail(err, len, "Unsupported BMP header");
        goto done;
    }
    if (!(comp == 0 || (comp == 3 && (bpp == 16 || bpp == 32))) ||
        !(bpp == 1 || bpp == 4 || bpp == 8 || bpp == 16 || bpp == 24 || bpp == 32)) {
        fail(err, len, "Compressed BMPs are not supported");
        goto done;
    }
    uint32_t masks[3] = {0x00FF0000, 0x0000FF00, 0x000000FF};
    if (bpp == 16 && comp == 0) {
        masks[0] = 0x7C00, masks[1] = 0x03E0, masks[2] = 0x001F;
    }
    if (comp == 3) {
        const uint8_t *m = bytes_at(r, 54, 12); // after a 40-byte header, else inside a V4/V5 one
        if (!m) {
            fail(err, len, "Damaged BMP file");
            goto done;
        }
        for (int k = 0; k < 3; k++) {
            masks[k] = le32(m + 4 * k);
        }
    }
    int shift[3], bits[3];
    for (int k = 0; k < 3; k++) {
        mask_shift(masks[k], &shift[k], &bits[k]);
    }

    int colors = 0;
    if (bpp <= 8) {
        colors = used && used < (1u << bpp) ? (int)used : 1 << bpp;
        if (!(pal = malloc(256 * sizeof(uint32_t)))) {
            fail(err, len, "Not enough memory");
            goto done;
        }
        memset(pal, 0, 256 * sizeof(uint32_t));
        for (int i = 0; i < colors; i++) {
            const uint8_t *e = bytes_at(r, 14 + dib + 4 * i, 4);
            if (!e) {
                fail(err, len, "Damaged BMP file");
                goto done;
            }
            pal[i] = (uint32_t)e[2] << 16 | (uint32_t)e[1] << 8 | e[0]; // 0xRRGGBB
        }
    }
    bool direct = bpp <= 4 || (bpp == 8 && colors <= 16);

    // the palette, and the background
    lcd_color_t bg = 0;
    if (direct) {
        int darkest = 1 << 30;
        for (int i = 0; i < 16; i++) {
            uint32_t c = i < colors ? pal[i] : 0;
            int r8 = c >> 16 & 0xFF, g8 = c >> 8 & 0xFF, b8 = c & 0xFF;
            lcd_set_palette(i, RGB565(r8, g8, b8));
            if (i < colors && r8 + g8 + b8 < darkest) {
                darkest = r8 + g8 + b8;
                bg = (lcd_color_t)i;
            }
        }
    } else {
        for (int i = 0; i < 16; i++) {
            int v = i * 17;
            lcd_set_palette(i, grey ? RGB565(v, v, v) : RGB565((i >> 3) * 255, ((i >> 1) & 3) * 85, (i & 1) * 255));
        }
    }

    // fit the screen: shrink, or grow by a whole factor
    int dw, dh;
    if (w > LCD_W || hh > LCD_H) {
        int f = (w + LCD_W - 1) / LCD_W, fh = (hh + LCD_H - 1) / LCD_H;
        f = fh > f ? fh : f;
        dw = w / f, dh = hh / f;
    } else {
        int f = LCD_W / w < LCD_H / hh ? LCD_W / w : LCD_H / hh;
        dw = w * f, dh = hh * f;
    }
    dw = dw < 1 ? 1 : dw;
    dh = dh < 1 ? 1 : dh;
    int x0 = (LCD_W - dw) / 2, y0 = (LCD_H - dh) / 2;
    lcd_fill_rect(0, 0, LCD_W, LCD_H, bg);
    lcd_flush();

    uint32_t stride = ((uint32_t)bpp * (uint32_t)w + 31) / 32 * 4;
    int flushed = 0;
    for (int oy = 0; oy < dh; oy++) {
        int sy = (int)((int64_t)oy * hh / dh);
        uint32_t row = data + (uint32_t)(top_down ? sy : hh - 1 - sy) * stride;
        for (int ox = 0; ox < dw; ox++) {
            int sx = (int)((int64_t)ox * w / dw);
            uint32_t bit = (uint32_t)sx * (uint32_t)bpp;
            const uint8_t *p = bytes_at(r, row + bit / 8, bpp >= 8 ? (UINT)(bpp / 8) : 1);
            if (!p) {
                break; // a short file: show what there is
            }
            int idx = 0, r8, g8, b8;
            if (bpp < 8) {
                idx = (*p >> (8 - bpp - (int)(bit % 8))) & ((1 << bpp) - 1);
            } else if (bpp == 8) {
                idx = *p;
            }
            if (direct) {
                lcd_pixel(x0 + ox, y0 + oy, (lcd_color_t)idx);
                continue;
            }
            if (bpp == 8) {
                r8 = pal[idx] >> 16 & 0xFF, g8 = pal[idx] >> 8 & 0xFF, b8 = pal[idx] & 0xFF;
            } else if (bpp == 24) {
                r8 = p[2], g8 = p[1], b8 = p[0];
            } else {
                uint32_t px = bpp == 16 ? le16(p) : le32(p);
                r8 = channel(px, masks[0], shift[0], bits[0]);
                g8 = channel(px, masks[1], shift[1], bits[1]);
                b8 = channel(px, masks[2], shift[2], bits[2]);
            }
            int t = BAYER[(y0 + oy) & 3][(x0 + ox) & 3], c;
            if (grey) {
                c = dither((r8 * 77 + g8 * 150 + b8 * 29) >> 8, 16, t);
            } else {
                c = dither(r8, 2, t) << 3 | dither(g8, 4, t) << 1 | dither(b8, 2, t);
            }
            lcd_pixel(x0 + ox, y0 + oy, (lcd_color_t)c);
        }
        if (oy - flushed >= 15 || oy == dh - 1) { // show it as it comes
            lcd_flush_rows(y0 + flushed, oy + 1 - flushed);
            flushed = oy + 1;
        }
    }
    ok = true;
done:
    f_close(&r->f);
    free(pal);
    free(r);
    return ok;
}

// ---- screenshots ------------------------------------------------------------------------------
static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v), put16(p + 2, v >> 16);
}

bool shot_save(char *out, size_t len)
{
    f_mkdir(PML_DIR); // FR_EXIST once it is there
    f_mkdir(SHOT_DIR);
    FILINFO fi;
    int n = 1;
    for (; n < 1000; n++) {
        snprintf(out, len, SHOT_DIR "/shot-%03d.bmp", n);
        if (f_stat(out, &fi) != FR_OK) {
            break;
        }
    }
    FIL *f = malloc(sizeof(FIL));
    if (n == 1000 || !f || f_open(f, out, FA_WRITE | FA_CREATE_NEW) != FR_OK) {
        free(f);
        return false;
    }
    // 14-byte file header, 40-byte info header, 16 palette entries, then the rows bottom-up
    enum { HDR = 14 + 40 + 16 * 4, ROW = LCD_W / 2 };
    uint8_t buf[HDR > ROW ? HDR : ROW];
    memset(buf, 0, HDR);
    buf[0] = 'B', buf[1] = 'M';
    put32(buf + 2, HDR + ROW * LCD_H);
    put32(buf + 10, HDR);
    put32(buf + 14, 40);
    put32(buf + 18, LCD_W);
    put32(buf + 22, LCD_H);
    put16(buf + 26, 1);
    put16(buf + 28, 4);
    put32(buf + 34, ROW * LCD_H);
    put32(buf + 46, 16);
    for (int i = 0; i < 16; i++) {
        uint16_t c = lcd_get_palette((lcd_color_t)i);
        uint8_t *e = buf + 54 + 4 * i;
        e[0] = (uint8_t)((c & 0x1F) * 255 / 31);
        e[1] = (uint8_t)((c >> 5 & 0x3F) * 255 / 63);
        e[2] = (uint8_t)((c >> 11) * 255 / 31);
    }
    UINT wrote;
    bool ok = f_write(f, buf, HDR, &wrote) == FR_OK && wrote == HDR;
    for (int y = LCD_H - 1; ok && y >= 0; y--) {
        const uint8_t *src = lcd_row(y);
        for (int i = 0; i < ROW; i++) { // the framebuffer has the left pixel in the low nibble
            buf[i] = (uint8_t)(src[i] << 4 | src[i] >> 4);
        }
        ok = f_write(f, buf, ROW, &wrote) == FR_OK && wrote == ROW;
    }
    ok = f_close(f) == FR_OK && ok;
    free(f);
    if (!ok) {
        f_unlink(out);
    }
    return ok;
}
