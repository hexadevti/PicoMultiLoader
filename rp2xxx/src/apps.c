#include "apps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "pico/stdlib.h"
#include "ff.h"
#include "lcd.h"
#include "look.h"
#include "sdcard.h"
#include "uf2_image.h"

#if PICO_RP2040
#define BOARD "rp2040"
#else
#define BOARD "rp2350"
#endif
#define INI_MAX    512
#define PATH_CAP   300
#define RECENT_MAX 8

static const char *const CAT_NAMES[APP_CATS] = {"", "Emulators", "Tools", "Dev", "Media"};

// An app.ini's text (or an icon's pixels) and a path, in the UF2 reader's buffer: it is idle here.
static char *scratch(void)
{
    size_t len;
    return uf2_scratch(&len);
}
#define s_buf  scratch()
#define s_path (scratch() + INI_MAX + 1)

const char *apps_cat_name(int cat)
{
    return cat > 0 && cat < APP_CATS ? CAT_NAMES[cat] : "";
}

static bool has_uf2_ext(const char *name)
{
    size_t len = strlen(name);
    return len > 4 && strcasecmp(name + len - 4, ".uf2") == 0;
}

static bool hidden(const FILINFO *fi)
{
    return fi->fname[0] == '.' || (fi->fattrib & (AM_HID | AM_SYS)) ||
           strcasecmp(fi->fname, "System Volume Information") == 0;
}

// ---- path lists (favourites, recents) ---------------------------------------------------------
// The file's lines, each ending in '\n', in memory.
typedef struct {
    const char *file;
    char *buf;
    uint16_t cap;
    bool loaded;
} plist_t;

static char s_fav_buf[384], s_rec_buf[256];
static plist_t s_favs = {PML_DIR "/favourites.txt", s_fav_buf, sizeof(s_fav_buf), false};
static plist_t s_recents = {PML_DIR "/recents.txt", s_rec_buf, sizeof(s_rec_buf), false};

static void pl_load(plist_t *l)
{
    if (l->loaded) {
        return;
    }
    l->loaded = true;
    l->buf[0] = '\0';
    FIL f;
    if (f_open(&f, l->file, FA_READ) != FR_OK) {
        return;
    }
    size_t len = 0;
    while (f_gets(s_path, PATH_CAP, &f)) {
        s_path[strcspn(s_path, "\r\n")] = '\0';
        size_t n = strlen(s_path);
        if (s_path[0] == '/' && len + n + 2 <= l->cap) {
            memcpy(l->buf + len, s_path, n);
            len += n;
            l->buf[len++] = '\n';
        }
    }
    l->buf[len] = '\0';
    f_close(&f);
}

static char *pl_find(plist_t *l, const char *path)
{
    pl_load(l);
    size_t n = strlen(path);
    for (char *s = l->buf; *s; s = strchr(s, '\n') + 1) {
        if (strncasecmp(s, path, n) == 0 && s[n] == '\n') {
            return s;
        }
    }
    return NULL;
}

static int pl_index(plist_t *l, const char *path)
{
    char *at = pl_find(l, path);
    int i = 0;
    for (char *s = l->buf; at && s < at; s = strchr(s, '\n') + 1) {
        i++;
    }
    return at ? i : -1;
}

static void pl_cut(char *line)
{
    char *next = strchr(line, '\n') + 1;
    memmove(line, next, strlen(next) + 1);
}

static bool pl_save(plist_t *l)
{
    f_mkdir(PML_DIR); // FR_EXIST once it is there
    FIL f;
    UINT n;
    if (f_open(&f, l->file, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        return false;
    }
    size_t len = strlen(l->buf);
    bool ok = f_write(&f, l->buf, len, &n) == FR_OK && n == len;
    return f_close(&f) == FR_OK && ok;
}

bool apps_is_fav(const char *path)
{
    return pl_find(&s_favs, path) != NULL;
}

int apps_toggle_fav(const char *path)
{
    char *at = pl_find(&s_favs, path);
    size_t len = strlen(s_favs.buf), n = strlen(path);
    if (at) {
        pl_cut(at);
    } else if (path[0] != '/' || len + n + 2 > s_favs.cap) {
        return -1; // full
    } else {
        memcpy(s_favs.buf + len, path, n);
        strcpy(s_favs.buf + len + n, "\n");
    }
    if (!pl_save(&s_favs)) {
        s_favs.loaded = false; // back to what the card has
        return -1;
    }
    return !at;
}

void apps_add_recent(const char *path)
{
    char *at = pl_find(&s_recents, path);
    size_t n = strlen(path);
    if (at) {
        pl_cut(at);
    }
    if (path[0] != '/' || n + 2 > s_recents.cap) {
        return;
    }
    // Drop the oldest until it fits and there are at most RECENT_MAX
    for (;;) {
        int lines = 0;
        char *last = NULL;
        for (char *s = s_recents.buf; *s; s = strchr(s, '\n') + 1) {
            lines++;
            last = s;
        }
        if (!last || (lines < RECENT_MAX && strlen(s_recents.buf) + n + 2 <= s_recents.cap)) {
            break;
        }
        *last = '\0';
    }
    memmove(s_recents.buf + n + 1, s_recents.buf, strlen(s_recents.buf) + 1);
    memcpy(s_recents.buf, path, n);
    s_recents.buf[n] = '\n';
    if (!pl_save(&s_recents)) {
        printf("apps: could not save %s\n", s_recents.file);
    }
}

void apps_reload(void)
{
    s_favs.loaded = s_recents.loaded = false;
}

// ---- app.ini ----------------------------------------------------------------------------------
static bool read_ini(const char *dir)
{
    FIL f;
    UINT n = 0;
    snprintf(s_path, PATH_CAP, "%s/app.ini", dir);
    if (f_open(&f, sd_path(s_path), FA_READ) != FR_OK) {
        return false;
    }
    bool ok = f_read(&f, s_buf, INI_MAX, &n) == FR_OK;
    f_close(&f);
    s_buf[ok ? n : 0] = '\0';
    return ok;
}

static bool blank(char c)
{
    return c == ' ' || c == '\t';
}

// "key = value" from the app.ini in s_buf; [sections] are ignored.
static bool ini_get(const char *key, char *out, size_t len)
{
    size_t kl = strlen(key);
    for (const char *l = s_buf; *l;) {
        const char *end = strchr(l, '\n');
        end = end ? end : l + strlen(l);
        while (l < end && blank(*l)) {
            l++;
        }
        if ((size_t)(end - l) > kl && strncasecmp(l, key, kl) == 0) {
            const char *p = l + kl;
            while (p < end && blank(*p)) {
                p++;
            }
            if (p < end && *p == '=') {
                for (p++; p < end && blank(*p); p++) {
                }
                const char *e = p;
                while (e < end && *e != '\r' && !(*e == ';' && (e == p || blank(e[-1])))) {
                    e++;
                }
                while (e > p && blank(e[-1])) {
                    e--;
                }
                snprintf(out, len, "%.*s", (int)(e - p), p);
                return true;
            }
        }
        l = *end ? end + 1 : end;
    }
    return false;
}

static bool board_ok(const char *boards)
{
    char low[40];
    size_t i = 0;
    for (; boards[i] && i + 1 < sizeof(low); i++) {
        low[i] = (char)(boards[i] >= 'A' && boards[i] <= 'Z' ? boards[i] + 32 : boards[i]);
    }
    low[i] = '\0';
    return !low[0] || strstr(low, "any") || strstr(low, BOARD);
}

static int cat_of(const char *name)
{
    for (int c = 1; c < APP_CATS; c++) {
        if (strcasecmp(name, CAT_NAMES[c]) == 0) {
            return c;
        }
    }
    return APP_CAT_NONE;
}

// ---- scanning ---------------------------------------------------------------------------------
static uint16_t add_str(apps_t *a, const char *s)
{
    size_t n = strlen(s) + 1;
    if (!s[0] || a->used + n > APPS_POOL) {
        return APP_NONE;
    }
    memcpy(a->pool + a->used, s, n);
    a->used += (uint16_t)n;
    return (uint16_t)(a->used - n);
}

const char *apps_str(const apps_t *a, uint16_t off)
{
    return off == APP_NONE ? "" : a->pool + off;
}

static bool add_app(apps_t *a, const char *path, const char *title, const char *binary, const char *icon, int cat,
                    uint8_t flags)
{
    uint16_t used = a->used;
    app_t *e = &a->list[a->count];
    if (a->count >= APPS_MAX || (e->path = add_str(a, path)) == APP_NONE || (e->title = add_str(a, title)) == APP_NONE) {
        a->used = used;
        return false;
    }
    e->binary = add_str(a, binary);
    e->icon = add_str(a, icon);
    e->cat = (uint8_t)cat;
    e->flags = flags;
    e->recent = -1;
    a->count++;
    return true;
}

// The one .uf2 in a folder: returns how many there are (0, 1, or 2 for "several").
static int find_uf2(const char *dir, char *out, size_t len)
{
    DIR d;
    FILINFO fi;
    int n = 0;
    if (f_opendir(&d, sd_path(dir)) != FR_OK) {
        return 0;
    }
    while (n < 2 && f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        if (!(fi.fattrib & AM_DIR) && !hidden(&fi) && has_uf2_ext(fi.fname)) {
            size_t l = strlen(fi.fname);
            if (!n++ && l < len) {
                memcpy(out, fi.fname, l + 1);
            } else if (l >= len) {
                n = 2; // too long a name to keep: open the folder instead
            }
        }
    }
    f_closedir(&d);
    return n;
}

// A folder with an app.ini or a .uf2 in it (extras: any folder).
static void add_dir(apps_t *a, const char *path, uint8_t flags)
{
    char title[48], bin[64] = "", icon[32] = "", cat[16] = "", board[40];
    const char *name = strrchr(path, '/') + 1;
    bool ini = read_ini(path);
    if (ini) {
        if (ini_get("board", board, sizeof(board)) && !board_ok(board)) {
            return; // for another board
        }
        if (!ini_get("name", title, sizeof(title)) || !title[0]) {
            snprintf(title, sizeof(title), "%s", name);
        }
        if (!ini_get("binary." BOARD, bin, sizeof(bin))) {
            ini_get("binary", bin, sizeof(bin));
        }
        if (!has_uf2_ext(bin) || strchr(bin, '/')) {
            bin[0] = '\0'; // e.g. the P4's .bin
        }
        if (ini_get("icon", icon, sizeof(icon)) && (strcasecmp(icon, "icon.bmp") == 0 || strchr(icon, '/'))) {
            icon[0] = '\0';
        }
        ini_get("category", cat, sizeof(cat));
    } else {
        snprintf(title, sizeof(title), "%s", name);
    }
    if (!bin[0]) {
        int n = find_uf2(path, bin, sizeof(bin));
        if (n == 0 && !ini && !(flags & APP_EXTRA)) {
            return; // just a folder
        }
        if (n != 1) {
            bin[0] = '\0';
        }
    }
    add_app(a, path, title, bin, icon, cat_of(cat), flags | APP_DIR);
}

static void add_file(apps_t *a, const char *path, uint8_t flags)
{
    char title[48];
    const char *name = strrchr(path, '/') + 1;
    snprintf(title, sizeof(title), "%.*s", (int)strlen(name) - 4, name);
    add_app(a, path, title, "", "", APP_CAT_NONE, flags);
}

bool apps_binary_path(const apps_t *a, const app_t *e, char *out, size_t len)
{
    if (!(e->flags & APP_DIR)) {
        snprintf(out, len, "%s", apps_str(a, e->path));
        return true;
    }
    if (e->binary == APP_NONE) {
        return false;
    }
    return (size_t)snprintf(out, len, "%s/%s", apps_str(a, e->path), apps_str(a, e->binary)) < len;
}

// `path` (from a list file) is this app, or the .uf2 it runs.
static bool is_app_path(const apps_t *a, const app_t *e, const char *path)
{
    const char *p = apps_str(a, e->path);
    size_t n = strlen(p);
    if (strcasecmp(p, path) == 0) {
        return true;
    }
    return e->binary != APP_NONE && strncasecmp(p, path, n) == 0 && path[n] == '/' &&
           strcasecmp(path + n + 1, apps_str(a, e->binary)) == 0;
}

static const apps_t *s_sorting;

static int cmp_apps(const void *x, const void *y)
{
    const app_t *p = x, *q = y;
    return strcasecmp(apps_str(s_sorting, p->title), apps_str(s_sorting, q->title));
}

// Mark the apps that a list names, and (`add`) add the ones it names that aren't in the root.
static void merge_list(apps_t *a, plist_t *l, bool fav, bool add)
{
    pl_load(l);
    int index = 0;
    for (const char *s = l->buf; *s; s = strchr(s, '\n') + 1, index++) {
        char path[PATH_CAP];
        size_t n = strcspn(s, "\n");
        if (n >= sizeof(path)) {
            continue;
        }
        memcpy(path, s, n);
        path[n] = '\0';
        int i = 0;
        while (i < a->count && !is_app_path(a, &a->list[i], path)) {
            i++;
        }
        if (i == a->count) { // not in the root: add it if it is still there
            FILINFO fi;
            if (!add || f_stat(sd_path(path), &fi) != FR_OK) {
                continue;
            }
            if (fi.fattrib & AM_DIR) {
                add_dir(a, path, APP_EXTRA);
            } else if (has_uf2_ext(path)) {
                add_file(a, path, APP_EXTRA);
            }
            if (i == a->count) {
                continue; // not an app after all, or no room
            }
        }
        if (fav) {
            a->list[i].flags |= APP_FAV;
        } else if (a->list[i].recent < 0) {
            a->list[i].recent = (int8_t)index;
        }
    }
}

void apps_scan(apps_t *a)
{
    apps_scan_dir(a, "");
}

void apps_scan_dir(apps_t *a, const char *dir)
{
    bool root = !dir[0];
    a->count = a->used = 0;
    DIR d;
    FILINFO fi;
    if (f_opendir(&d, sd_path(dir)) == FR_OK) {
        while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
            char path[PATH_CAP];
            if (hidden(&fi) || (root && strcasecmp(fi.fname, PML_DIR + 1) == 0) ||
                (size_t)snprintf(path, sizeof(path), "%s/%s", dir, fi.fname) >= sizeof(path)) {
                continue;
            }
            if (fi.fattrib & AM_DIR) {
                add_dir(a, path, root ? 0 : APP_EXTRA);
            } else if (has_uf2_ext(fi.fname)
#if !PICO_RP2040
                       && !(root && strcasecmp(fi.fname, "pml2350.uf2") == 0) // the loader itself
#endif
            ) {
                add_file(a, path, root ? 0 : APP_EXTRA);
            }
        }
        f_closedir(&d);
    }
    s_sorting = a;
    qsort(a->list, a->count, sizeof(a->list[0]), cmp_apps);
    merge_list(a, &s_favs, true, root);
    merge_list(a, &s_recents, false, root);
}

bool apps_info(const apps_t *a, const app_t *e, app_info_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!(e->flags & APP_DIR) || !read_ini(apps_str(a, e->path))) {
        return false;
    }
    ini_get("version", out->version, sizeof(out->version));
    ini_get("author", out->author, sizeof(out->author));
    ini_get("desc", out->desc, sizeof(out->desc));
    return true;
}

// ---- icons ------------------------------------------------------------------------------------
static uint32_t le32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

// A 32x32 4-bit BMP; its palette is ignored: pixel values are palette roles, 0 is see-through.
static bool draw_bmp(const char *path, int x, int y)
{
    FIL f;
    UINT n = 0;
    uint8_t *b = (uint8_t *)s_buf;
    if (f_open(&f, sd_path(path), FA_READ) != FR_OK) {
        return false;
    }
    bool ok = f_read(&f, b, 54, &n) == FR_OK && n == 54 && b[0] == 'B' && b[1] == 'M';
    int32_t h = ok ? (int32_t)le32(b + 22) : 0;
    ok = ok && le32(b + 18) == 32 && (h == 32 || h == -32) && (b[28] | b[29] << 8) == 4 && le32(b + 30) == 0 &&
         f_lseek(&f, le32(b + 10)) == FR_OK && f_read(&f, b, 512, &n) == FR_OK && n == 512;
    f_close(&f);
    if (!ok) {
        return false;
    }
    for (int r = 0; r < 32; r++) {
        const uint8_t *row = b + 16 * (h > 0 ? 31 - r : r); // rows are stored bottom-up unless h < 0
        for (int c = 0; c < 32;) {
            int v = row[c / 2] >> (c & 1 ? 0 : 4) & 15, start = c;
            while (c < 32 && (row[c / 2] >> (c & 1 ? 0 : 4) & 15) == v) {
                c++;
            }
            if (v) {
                lcd_fill_rect(x + start, y + r, c - start, 1, (lcd_color_t)v);
            }
        }
    }
    return true;
}

// Generic icons: a folder, or a window with the app's initial.
static void draw_generic(const apps_t *a, const app_t *e, int x, int y)
{
    if ((e->flags & APP_DIR) && e->binary == APP_NONE) {
        lcd_fill_rect(x + 2, y + 6, 12, 4, C_ACCENT);
        lcd_fill_rect(x + 2, y + 9, 28, 19, C_ACCDK);
        look_frame(x + 2, y + 9, 28, 19, C_ACCENT);
        return;
    }
    lcd_fill_rect(x + 2, y + 3, 28, 26, C_FIELD);
    look_frame(x + 2, y + 3, 28, 26, C_LINE);
    lcd_fill_rect(x + 3, y + 4, 26, 5, (e->flags & APP_DIR) ? C_ACCENT : C_ACC2);
    char initial[2] = {apps_str(a, e->title)[0], '\0'};
    if (initial[0] >= 'a' && initial[0] <= 'z') {
        initial[0] -= 32;
    }
    lcd_text(x + 13, y + 12, initial, C_BRIGHT, FONT_LARGE);
}

void apps_draw_icon(const apps_t *a, const app_t *e, int x, int y)
{
    if (e->flags & APP_DIR) {
        snprintf(s_path, PATH_CAP, "%s/%s", apps_str(a, e->path),
                 e->icon == APP_NONE ? "icon.bmp" : apps_str(a, e->icon));
        if (draw_bmp(s_path, x, y)) {
            return;
        }
    }
    draw_generic(a, e, x, y);
}
