// ui.c - on-screen launcher: Home (the apps on the SD card) and the file list (F3). Pick a .uf2,
// confirm, flash it and boot it; the app that is already installed starts without flashing.
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "pico/stdlib.h"
#include "apps.h"
#include "clock.h"
#include "config.h"
#include "fileops.h"
#include "keyboard.h"
#include "lcd.h"
#include "look.h"
#include "sdcard.h"
#include "shell.h"
#include "uf2_image.h"
#include "usb_drive.h"
#include "viewer.h"
#if PICOMULTILOADER_WIFI
#include "webserver.h"
#endif

// ---- layout -----------------------------------------------------------------------------------
// The colours are palette roles (look.h); the theme fills them in.
#define MARGIN    8
#define BAR_H     16 // status bar; a rule below it
#define PATH_Y    22
#define LIST_Y    46
#define ROW_H     15
#define FOOTER_Y  (LCD_H - 34)
#define KEYS_Y    (LCD_H - 18) // key bar; a rule above it
#define LIST_ROWS ((FOOTER_Y - 4 - LIST_Y) / ROW_H)
#define COLS      ((LCD_W - 2 * MARGIN) / 7)

#define MAX_ENTRIES 256
#define PATH_MAX_LEN 256

// ---- state ------------------------------------------------------------------------------------
typedef struct {
    char *name;
    bool is_dir;
    bool fav; // in the favourites
    uint32_t size;
} entry_t;

typedef enum {
    SCREEN_HOME,      // the apps (the start screen)
    SCREEN_BROWSE,    // F3: the file list
    SCREEN_CONFIRM,
    SCREEN_ERROR,
    SCREEN_USB,
    SCREEN_WEB,
    SCREEN_WIFI,      // Setup, from the control center (the Wi-Fi network, start folder, help)
    SCREEN_WIFI_SCAN, // pick a network from a scan
    SCREEN_HELP,      // how to use the loader (from Setup)
    SCREEN_PICK,      // choose the start folder (from Setup)
    SCREEN_FILES,     // Tab: copy / move / delete / paste
    SCREEN_DELETE,    // confirm a delete
    SCREEN_RES,       // RP2350: confirm installing the Wi-Fi files over an app's data
    SCREEN_CTL,       // F1: the control center (Wi-Fi, USB drive, backlight, theme, battery)
    SCREEN_TERM,      // F5: the terminal
    SCREEN_TEXT,      // the text reader (.txt, .md, ...)
    SCREEN_IMAGE,     // the BMP viewer
} screen_t;

// RP2350: the Wi-Fi firmware and the web page live in the last 256 KB of flash (uf2_image.h)
#if PICOMULTILOADER_WIFI && !PICO_RP2040
#define PML_RES 1
#else
#define PML_RES 0
#endif

static screen_t s_screen;
static screen_t s_base; // SCREEN_HOME or SCREEN_BROWSE: where Esc goes back to
static bool s_sd_ok;
static char s_cwd[PATH_MAX_LEN]; // absolute FatFS path, "" for the root, e.g. "/apps"
// The file list and Home's apps share the memory: only the one on s_base is kept.
static union {
    entry_t files[MAX_ENTRIES];
    apps_t apps;
} s_list;
#define s_entries s_list.files
#define s_apps    s_list.apps
static int s_count; // entries in s_entries; 0 while s_apps is in use
static int s_shown_n; // apps shown on Home; 0 while s_entries is in use
static int s_sel, s_top;
static int s_battery = -1;
static bool s_charging;

static char s_sel_path[PATH_MAX_LEN + 256];
static char s_sel_name[64];
static uf2_info_t s_info;
static bool s_confirm_run; // the confirm screen's file is the installed app: Enter just starts it
static char s_error[128];
static const char *s_path_override; // shown in the header instead of s_cwd

static char s_clip[PATH_MAX_LEN + 64]; // file or folder marked with Copy or Cut, "" = none
static bool s_clip_cut;                // Cut: Paste moves it instead of copying
static char s_note[80];                // result of the last file operation, shown in the footer
static lcd_color_t s_note_color;
static char s_toast[48];               // a message over the screen for a moment, "" = none
static uint64_t s_toast_until;
static int s_boot_line = -1;           // checklist lines on the boot screen, -1 = not showing it

// ---- helpers ----------------------------------------------------------------------------------
static bool at_root(void)
{
    return strcmp(s_cwd, SDCARD_MOUNT_POINT) == 0;
}

static bool load_setting(const char *key, char *buf, size_t len)
{
    return s_sd_ok && config_get(key, buf, len);
}

static void save_setting(const char *key, const char *value)
{
    if (s_sd_ok && !config_set(key, value)) {
        printf("ui: could not save %s\n", key);
    }
}

// True if `path` is a folder ("" and "/" are the root, which f_stat() can't look at).
static bool is_dir(const char *path)
{
    if (!path[0] || strcmp(path, "/") == 0) {
        return s_sd_ok;
    }
    FILINFO fi;
    return f_stat(path, &fi) == FR_OK && (fi.fattrib & AM_DIR);
}

// Folders are kept as "" for the root in memory, "/" in the settings file (empty = unset).
static void copy_dir(char *out, size_t len, const char *path)
{
    strlcpy(out, strcmp(path, "/") == 0 ? "" : path, len);
}

// Copy `s` into `out` (cols+1 bytes), cutting it to `cols` characters with a trailing "..".
static void fit(char *out, const char *s, int cols)
{
    int len = (int)strlen(s);
    if (len <= cols) {
        strcpy(out, s);
    } else {
        memcpy(out, s, cols - 2);
        strcpy(out + cols - 2, "..");
    }
}

static void format_size(char *out, size_t len, uint32_t bytes)
{
    uint32_t kb = (bytes + 1023) / 1024;
    if (kb < 10000) {
        snprintf(out, len, "%uKB", (unsigned)kb);
    } else {
        snprintf(out, len, "%uMB", (unsigned)(kb / 1024));
    }
}

// ---- directory listing ------------------------------------------------------------------------
static int cmp_entries(const void *a, const void *b)
{
    const entry_t *x = a, *y = b;
    if (x->is_dir != y->is_dir) {
        return x->is_dir ? -1 : 1;
    }
    return strcasecmp(x->name, y->name);
}

static bool has_uf2_ext(const char *name)
{
    size_t len = strlen(name);
    return len > 4 && strcasecmp(name + len - 4, ".uf2") == 0;
}

// Hidden files, macOS "._" files and "System Volume Information" aren't listed.
static bool hidden(const FILINFO *fi)
{
    return fi->fname[0] == '.' || (fi->fattrib & (AM_HID | AM_SYS)) ||
           strcasecmp(fi->fname, "System Volume Information") == 0;
}

static void free_entries(void)
{
    for (int i = 0; i < s_count; i++) {
        free(s_entries[i].name);
    }
    s_count = 0;
}

static void scan_dir(void)
{
    free_entries();
    s_shown_n = 0; // the apps are gone
    s_sel = s_top = 0;
    if (!s_sd_ok) {
        return;
    }
    DIR dir;
    if (f_opendir(&dir, sd_path(s_cwd)) != FR_OK) {
        printf("ui: cannot open %s\n", sd_path(s_cwd));
        return;
    }
    FILINFO fi;
    while (s_count < MAX_ENTRIES && f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
        bool dir_entry = (fi.fattrib & AM_DIR) != 0;
        if (hidden(&fi) || (!dir_entry && !has_uf2_ext(fi.fname) && view_kind(fi.fname) == VIEW_NONE)) {
            continue;
        }
        entry_t *e = &s_entries[s_count];
        char path[PATH_MAX_LEN + 64];
        bool fits = (size_t)snprintf(path, sizeof(path), "%s/%s", s_cwd, fi.fname) < sizeof(path);
        e->name = strdup(fi.fname);
        e->is_dir = dir_entry;
        e->fav = fits && apps_is_fav(path);
        e->size = dir_entry ? 0 : (uint32_t)fi.fsize;
        if (e->name) {
            s_count++;
        }
    }
    f_closedir(&dir);
    qsort(s_entries, s_count, sizeof(s_entries[0]), cmp_entries);
}

// Index 0 is ".." when not at the root; the rest map onto s_entries.
static int item_count(void)
{
    return s_count + (at_root() ? 0 : 1);
}

static entry_t *item_entry(int idx)
{
    int i = at_root() ? idx : idx - 1;
    return i >= 0 ? &s_entries[i] : NULL; // NULL = ".."
}

static void change_dir(const char *name)
{
    char prev[64] = "";
    if (name == NULL) { // go up
        char *slash = strrchr(s_cwd, '/');
        if (at_root() || !slash) {
            return;
        }
        strlcpy(prev, slash + 1, sizeof(prev));
        *slash = '\0';
    } else {
        size_t len = strlen(s_cwd);
        if (len + 1 + strlen(name) >= sizeof(s_cwd)) {
            return;
        }
        snprintf(s_cwd + len, sizeof(s_cwd) - len, "/%s", name);
    }
    scan_dir();
    // Coming back up: select the directory we came from.
    for (int i = 0; prev[0] && i < item_count(); i++) {
        entry_t *e = item_entry(i);
        if (e && e->is_dir && strcmp(e->name, prev) == 0) {
            s_sel = i;
            break;
        }
    }
    save_setting("last_dir", sd_path(s_cwd));
}

// ---- web server icon (status bar Wi-Fi bars, lit while the server is on) -----------------------
#if PICOMULTILOADER_WIFI
static lcd_color_t s_icon_shown; // colour of the bars last drawn, 0 (C_BG) = unlit

static bool web_running(void)
{
    web_status_t st;
    webserver_status(&st);
    return st.state != WEB_OFF;
}

// C_BG when the server is off; green when it's ready, blue while starting, amber while
// reconnecting, red if it failed.
static lcd_color_t web_icon_color(void)
{
    web_status_t st;
    webserver_status(&st);
    switch (st.state) {
    case WEB_OFF:
        return C_BG;
    case WEB_STARTING:
    case WEB_CONNECTING:
        return C_ACCENT;
    case WEB_RUNNING:
        return st.ip[0] ? C_OK : C_WARN;
    default:
        return C_ERR;
    }
}

#else
static bool web_running(void)
{
    return false;
}
#endif

// ---- drawing ----------------------------------------------------------------------------------
// Status bar (title, Wi-Fi, battery); clears the screen first.
// The clock in the middle of the status bar, once the time is known (set from the web)
#define BAR_CLOCK_X ((LCD_W - 5 * 6) / 2)
static int s_bar_minute = -1; // minute of the day shown, -1 = none

static int bar_minute(void)
{
    clock_tm_t tm;
    return clock_local(&tm) ? tm.hour * 60 + tm.min : -1;
}

static void draw_bar_clock(void)
{
    char text[12];
    s_bar_minute = bar_minute();
    lcd_fill_rect(BAR_CLOCK_X, 2, 5 * 6, 10, C_PANEL);
    if (s_bar_minute >= 0) {
        snprintf(text, sizeof(text), "%02d:%02d", s_bar_minute / 60, s_bar_minute % 60);
        lcd_text(BAR_CLOCK_X, 3, text, C_FG, FONT_SMALL);
    }
}

static void draw_bar(void)
{
    look_anim_reset();
    lcd_fill_rect(0, 0, LCD_W, LCD_H, C_BG);
    lcd_fill_rect(0, 0, LCD_W, BAR_H, C_PANEL);
    lcd_hline(0, BAR_H, LCD_W, C_LINE);
    lcd_text(6, 3, "PicoMultiLoader", C_FG, FONT_SMALL);
    draw_bar_clock();

    int x = LCD_W - 6; // right edge of the next item, filled right to left
    if (s_battery >= 0) {
        // battery: 16x8 body + 2x4 nub, fill proportional to charge
        char pct[12];
        snprintf(pct, sizeof(pct), "%d%%", s_battery);
        x -= 6 * (int)strlen(pct);
        lcd_text(x, 3, pct, C_FG, FONT_SMALL);
        x -= 21;
        look_frame(x, 4, 16, 8, C_DIM);
        lcd_fill_rect(x + 16, 6, 2, 4, C_DIM);
        lcd_fill_rect(x + 2, 6, 12 * s_battery / 100, 4, s_charging ? C_OK : s_battery < 15 ? C_ERR : C_FG);
        x -= 6;
    }
#if PICOMULTILOADER_WIFI
    s_icon_shown = web_icon_color();
    x -= 11;
    for (int i = 0; i < 4; i++) {
        lcd_fill_rect(x + 3 * i, 9 - 2 * i, 2, 3 + 2 * i, s_icon_shown == C_BG ? C_LINE : s_icon_shown);
    }
#endif
}

// The status bar and the path line.
static void draw_header(void)
{
    draw_bar();
    char path[COLS + 1], shown[PATH_MAX_LEN + 8];
    snprintf(shown, sizeof(shown), "Path: SD%s", s_path_override ? s_path_override : s_cwd);
    if (strlen(shown) == strlen("Path: SD")) {
        strcat(shown, "/");
    }
    fit(path, shown, COLS);
    lcd_text(MARGIN, PATH_Y, path, C_FG, FONT_LARGE);
    lcd_hline(MARGIN - 4, PATH_Y + 16, LCD_W - 2 * (MARGIN - 4), C_LINE);
}

static const char *const KEY_NAMES[] = {"Enter", "Esc", "Left", "Del", "Tab", "R", "Up/Down", "Left/Right", "PgUp/PgDn", "Home/End"};

// `keys` is "Enter Open  F1 Control  Esc Back": items two spaces apart, each a key and what it does.
// Function keys get a raised cap; an item that doesn't start with a key is shown as it is.
static void draw_footer(const char *line1, lcd_color_t line1_color, const char *keys)
{
    lcd_fill_rect(0, FOOTER_Y + 1, LCD_W, LCD_H - FOOTER_Y - 1, C_BG);
    if (line1) {
        lcd_text(MARGIN, FOOTER_Y + 3, line1, line1_color, FONT_SMALL);
    }
    lcd_hline(0, KEYS_Y, LCD_W, C_LINE);
    lcd_fill_rect(0, KEYS_Y + 1, LCD_W, LCD_H - KEYS_Y - 1, C_PANEL);
    int x = 6, y = KEYS_Y + 5;
    while (*keys) {
        char item[48];
        const char *end = strstr(keys, "  ");
        size_t n = end ? (size_t)(end - keys) : strlen(keys);
        strlcpy(item, keys, n < sizeof(item) ? n + 1 : sizeof(item));
        for (keys += n; *keys == ' '; keys++) {
        }
        char *label = strchr(item, ' ');
        bool fkey = item[0] == 'F' && item[1] >= '1' && item[1] <= '9' && label == item + 2, key = fkey;
        if (label) {
            *label++ = '\0';
            for (size_t k = 0; k < sizeof(KEY_NAMES) / sizeof(KEY_NAMES[0]); k++) {
                key |= strcmp(item, KEY_NAMES[k]) == 0;
            }
        }
        if (fkey) { // its number, then what it does on a cap
            x = lcd_text(x, y, item + 1, C_DIM, FONT_SMALL) + 1;
            int w = 6 * (int)strlen(label) + 4;
            lcd_fill_rect(x, KEYS_Y + 3, w, 13, C_FIELD);
            lcd_text(x + 2, y, label, C_FG, FONT_SMALL);
            x += w + 6;
        } else if (key) {
            x = lcd_text(x, y, item, C_ACCENT, FONT_SMALL) + 4;
            x = lcd_text(x, y, label, C_DIM, FONT_SMALL) + 8;
        } else {
            if (label) {
                label[-1] = ' ';
            }
            x = lcd_text(x, y, item, C_FG, FONT_SMALL) + 8;
        }
    }
}

static void draw_browse(void);
static void draw_home(void);
static void reload_base(void);

// A dialog over the dimmed Home or file list, with a striped top edge. Content starts at LIST_Y + 4.
static void draw_dialog(int h)
{
    if (s_base == SCREEN_HOME) {
        draw_home();
    } else {
        draw_browse();
    }
    look_dither(BAR_H + 1, LCD_H - BAR_H - 1);
    int y = LIST_Y - 8;
    lcd_fill_rect(2, y, LCD_W - 4, h, C_PANEL);
    look_frame(2, y, LCD_W - 4, h, C_LINE);
    look_stripes(3, y + 1, LCD_W - 6, 3);
}

static void toast(const char *msg)
{
    strlcpy(s_toast, msg, sizeof(s_toast));
    s_toast_until = time_us_64() + 1800000;
}

static void draw_toast(void)
{
    int w = 6 * (int)strlen(s_toast) + 20, x = (LCD_W - w) / 2, y = FOOTER_Y - 26;
    lcd_fill_rect(x, y, w, 20, C_FIELD);
    look_march(x, y, w, 20);
    lcd_text(x + 10, y + 5, s_toast, C_BRIGHT, FONT_SMALL);
}

static void draw_browse(void)
{
    draw_header();

    int n = item_count();
    if (!s_sd_ok) {
        lcd_text(MARGIN, LIST_Y + 2 * ROW_H, "No SD card found.", C_ERR, FONT_LARGE);
        lcd_text(MARGIN, LIST_Y + 4 * ROW_H, "Insert a FAT32 card and press", C_DIM, FONT_LARGE);
        lcd_text(MARGIN, LIST_Y + 5 * ROW_H, "Enter to retry.", C_DIM, FONT_LARGE);
    } else if (n == 0) {
        lcd_text(MARGIN, LIST_Y + 2 * ROW_H, "Nothing to open here.", C_DIM, FONT_LARGE);
    }

    if (s_sel < s_top) {
        s_top = s_sel;
    } else if (s_sel >= s_top + LIST_ROWS) {
        s_top = s_sel - LIST_ROWS + 1;
    }

    for (int row = 0; row < LIST_ROWS && s_top + row < n; row++) {
        int idx = s_top + row;
        int y = LIST_Y + row * ROW_H;
        bool sel = idx == s_sel;
        lcd_color_t fg = sel ? C_SEL_FG : C_FG;
        if (sel) {
            lcd_fill_rect(MARGIN - 4, y - 1, LCD_W - 2 * (MARGIN - 4), ROW_H, C_SEL_BG);
        }

        entry_t *e = item_entry(idx);
        char label[COLS + 1], buf[300];
        int size_cols = 0;
        if (!e) {
            lcd_text(MARGIN, y, "[..]", fg, FONT_LARGE);
        } else if (e->is_dir) {
            snprintf(buf, sizeof(buf), "[%s]", e->name);
            fit(label, buf, COLS - 2);
            lcd_text(MARGIN, y, label, fg, FONT_LARGE);
        } else {
            char size[12];
            format_size(size, sizeof(size), e->size);
            size_cols = (int)strlen(size) + 1;
            fit(label, e->name, COLS - size_cols - 2);
            lcd_text(MARGIN, y, label, sel || has_uf2_ext(e->name) ? fg : C_ACC2, FONT_LARGE); // documents in colour
            lcd_text(LCD_W - MARGIN - 7 * (size_cols - 1), y, size, sel ? C_SEL_FG : C_DIM, FONT_LARGE);
        }
        if (e && e->fav) {
            lcd_text(LCD_W - MARGIN - 7 * (size_cols + 1), y, "*", sel ? C_SEL_FG : C_WARN, FONT_LARGE);
        }
    }

    // scroll bar
    if (n > LIST_ROWS) {
        int track = LIST_ROWS * ROW_H;
        int h = track * LIST_ROWS / n;
        int y = LIST_Y + (track - h) * s_top / (n - LIST_ROWS);
        lcd_fill_rect(LCD_W - 3, y - 1, 2, h, C_DIM);
    }

    char name[40], version[24], installed[PATH_MAX_LEN + 96];
    lcd_color_t color = C_DIM;
    if (s_note[0]) {
        strlcpy(installed, s_note, sizeof(installed));
        color = s_note_color;
    } else if (s_clip[0]) {
        snprintf(installed, sizeof(installed), "%s %s - V pastes here, Esc forgets", s_clip_cut ? "Cut" : "Copied",
                 strrchr(s_clip, '/') + 1);
        color = C_ACCENT;
    } else if (uf2_installed(name, sizeof(name), version, sizeof(version))) {
        snprintf(installed, sizeof(installed), "Installed: %s %s", name, version);
    } else {
        snprintf(installed, sizeof(installed), "Installed: (none)");
    }
    char line[(LCD_W - 2 * MARGIN) / 6 + 1];
    fit(line, installed, (LCD_W - 2 * MARGIN) / 6);
    draw_footer(line, color,
#if PICOMULTILOADER_WIFI
                "Enter Open  Tab Actions  F2 Fav  F3 Home  F4 Web  F5 Term"
#else
                "Enter Open  Tab Actions  F2 Fav  F3 Home  F5 Terminal"
#endif
    );
}

static void draw_kv(int y, const char *key, const char *value)
{
    char v[COLS + 1];
    lcd_text(MARGIN, y, key, C_DIM, FONT_LARGE);
    fit(v, value, COLS - 10);
    lcd_text(MARGIN + 70, y, v, C_FG, FONT_LARGE);
}

static void draw_confirm(void)
{
    draw_dialog(156);
    char buf[96];
    int y = LIST_Y + 4;
    lcd_text(MARGIN, y, s_confirm_run ? "Run this app? It is installed." : "Flash and run this app?", C_ACCENT,
             FONT_LARGE);
    y += 2 * ROW_H;
    draw_kv(y, "File", s_sel_name);
    y += ROW_H;
    draw_kv(y, "Program", s_info.name[0] ? s_info.name : "(no name in the file)");
    y += ROW_H;
    draw_kv(y, "Version", s_info.version[0] ? s_info.version : "-");
    y += ROW_H;
    draw_kv(y, "Built", s_info.build_date[0] ? s_info.build_date : "-");
    y += ROW_H;
    format_size(buf, sizeof(buf), s_info.addr_hi - s_info.addr_lo);
    draw_kv(y, "Size", buf);
    y += ROW_H;
    draw_kv(y, "Target", uf2_family_name(s_info.family));
    y += ROW_H;
    snprintf(buf, sizeof(buf), "flash, %u of %u KB", (unsigned)((s_info.addr_hi - s_info.addr_lo + 1023) / 1024),
             (unsigned)(uf2_flash_capacity() / 1024));
    draw_kv(y, "Type", buf);
    draw_footer(NULL, C_FG, s_confirm_run ? "Enter Run  R Reflash  Esc Back" : "Enter Flash & run  Esc Back");
}

static void draw_wrapped(int y, const char *text, lcd_color_t color)
{
    char line[COLS + 1];
    size_t len = strlen(text);
    for (size_t pos = 0; pos < len; y += ROW_H) {
        size_t n = len - pos > COLS ? COLS : len - pos;
        if (pos + n < len) { // break at the last space
            size_t sp = n;
            while (sp > 0 && text[pos + sp] != ' ') {
                sp--;
            }
            if (sp > 0) {
                n = sp;
            }
        }
        memcpy(line, text + pos, n);
        line[n] = '\0';
        lcd_text(MARGIN, y, line, color, FONT_LARGE);
        pos += n;
        while (text[pos] == ' ') {
            pos++;
        }
    }
}

static void draw_error(void)
{
    draw_dialog(144);
    int y = LIST_Y + 4;
    lcd_text(MARGIN, y, "Can't run this file", C_ERR, FONT_LARGE);
    y += 2 * ROW_H;
    draw_kv(y, "File", s_sel_name);
    y += 2 * ROW_H;
    draw_wrapped(y, s_error, C_FG);
    draw_footer(NULL, C_FG, "Esc Back");
}

static usbdrive_state_t s_usb_shown = USBDRIVE_OFF;

static void draw_usb(void)
{
    usbdrive_state_t st = usbdrive_state();
    s_usb_shown = st;
    draw_header();
    int y = LIST_Y + 4;
    lcd_text(MARGIN, y, "USB drive mode", C_ACCENT, FONT_LARGE);
    y += 2 * ROW_H;
    char buf[48];
    snprintf(buf, sizeof(buf), "%u MB", (unsigned)usbdrive_size_mb());
    draw_kv(y, "SD card", buf);
    y += 2 * ROW_H;

    const char *status, *help;
    lcd_color_t color;
    switch (st) {
    case USBDRIVE_CONNECTED:
        status = "Connected to the PC";
        color = C_OK;
        help = "The SD card is now a drive on your PC. Copy your .uf2 files, then eject "
               "the drive on the PC to come back here.";
        break;
    case USBDRIVE_SUSPENDED:
        status = "Cable unplugged or PC asleep";
        color = C_ERR;
        help = "Plug the cable back in, or press Esc to leave USB drive mode.";
        break;
    default:
        status = "Waiting for the PC...";
        color = C_FG;
        help = "Connect the Pico's micro-USB port to the PC. The PicoCalc's USB-C port is serial only.";
        break;
    }
    lcd_text(MARGIN, y, "Status", C_DIM, FONT_LARGE);
    lcd_text(MARGIN + 70, y, status, color, FONT_LARGE);
    y += 2 * ROW_H;
    draw_wrapped(y, help, C_DIM);
    draw_footer(NULL, C_FG, st == USBDRIVE_CONNECTED ? "Esc Stop (eject on the PC first!)" : "Esc Back");
}

static void leave_usb(void)
{
    usbdrive_stop(); // remounts the card
    s_sd_ok = sdcard_mount();
    if (!s_sd_ok || !is_dir(s_cwd)) {
        strlcpy(s_cwd, SDCARD_MOUNT_POINT, sizeof(s_cwd));
    }
    config_reload(); // the PC may have changed anything
    apps_reload();
    reload_base();
    s_screen = s_base;
}

#if PICOMULTILOADER_WIFI
static void stop_web(void);
#endif

static void enter_usb(void)
{
#if PICOMULTILOADER_WIFI
    if (web_running()) {
        stop_web(); // the PC gets the raw card; the web server can't share it
    }
#endif
    free_entries(); // the listing goes stale while the PC owns the card
    s_shown_n = s_apps.count = 0;
    if (!usbdrive_start()) {
        s_sd_ok = sdcard_mount();
        reload_base();
        snprintf(s_error, sizeof(s_error), "Could not start USB drive mode: no SD card, or it did not answer.");
        strlcpy(s_sel_name, "(SD card)", sizeof(s_sel_name));
        s_screen = SCREEN_ERROR;
        return;
    }
    s_screen = SCREEN_USB;
}

// ---- web server mode (F4) ---------------------------------------------------------------------
#if PICOMULTILOADER_WIFI
static uint32_t s_web_rev_shown;
static uint64_t s_web_drawn_us;

static void draw_web(void)
{
    web_status_t st;
    s_web_rev_shown = webserver_status(&st);
    s_web_drawn_us = time_us_64();
    draw_header();
    int y = LIST_Y + 4;
    lcd_text(MARGIN, y, "Web file manager", C_ACCENT, FONT_LARGE);
    y += 2 * ROW_H;

    char buf[96];
    switch (st.state) {
    case WEB_STARTING:
        lcd_text(MARGIN, y, "Starting Wi-Fi...", C_FG, FONT_LARGE);
        break;
    case WEB_CONNECTING:
        draw_kv(y, "Network", st.ssid);
        y += ROW_H;
        lcd_text(MARGIN, y, "Status", C_DIM, FONT_LARGE);
        lcd_text(MARGIN + 70, y, "Connecting...", C_FG, FONT_LARGE);
        y += 2 * ROW_H;
        draw_wrapped(y, "If it can't join within 20 s, the loader starts its own Wi-Fi network instead.", C_DIM);
        break;
    case WEB_RUNNING:
        if (st.ap_mode) {
            draw_kv(y, "Wi-Fi", st.ssid);
            y += ROW_H;
            draw_kv(y, "Password", st.password);
            y += ROW_H;
        } else {
            draw_kv(y, "Network", st.ssid);
            y += ROW_H;
        }
        lcd_text(MARGIN, y, "Open", C_DIM, FONT_LARGE);
        if (st.ip[0]) {
            snprintf(buf, sizeof(buf), "http://%s/", st.ip);
            lcd_text(MARGIN + 70, y, buf, C_OK, FONT_LARGE);
            y += ROW_H;
            lcd_text(MARGIN + 70, y, "http://" WEB_HOSTNAME ".local/", C_FG, FONT_LARGE);
        } else {
            lcd_text(MARGIN + 70, y, "(reconnecting...)", C_ERR, FONT_LARGE);
        }
        y += 2 * ROW_H;
        if (st.activity[0]) {
            char act[COLS + 1];
            fit(act, st.activity, COLS);
            lcd_text(MARGIN, y, act, C_FG, FONT_LARGE);
            y += ROW_H + 2;
            if (st.progress >= 0) {
                int w = LCD_W - 2 * MARGIN;
                look_frame(MARGIN, y, w, 10, C_LINE);
                look_stripes(MARGIN + 2, y + 2, (w - 4) * st.progress / 100, 6);
            }
        }
        if (st.ap_mode) {
            draw_wrapped(FOOTER_Y - 3 * ROW_H, "Join this network, or save your own Wi-Fi in Setup (F1 > Setup) to use that instead.", C_DIM);
        }
        break;
    case WEB_FAILED:
    default:
        lcd_text(MARGIN, y, "Wi-Fi failed", C_ERR, FONT_LARGE);
        y += 2 * ROW_H;
        draw_wrapped(y, st.error, C_FG);
        break;
    }
    draw_footer(st.state == WEB_FAILED ? "The server is still on until you stop it." : "Esc leaves it running in the background.",
                C_DIM, st.state == WEB_FAILED ? "Esc Stop the server" : "Esc Back  F4 Stop server");
}

static uint32_t s_web_changes_seen;

static void stop_web(void)
{
    webserver_stop();
    s_web_changes_seen = webserver_changes();
    if (!s_sd_ok || !is_dir(s_cwd)) {
        strlcpy(s_cwd, SDCARD_MOUNT_POINT, sizeof(s_cwd));
    }
    config_reload(); // the browser may have changed anything
    apps_reload();
    reload_base();
    s_screen = s_base;
}

#if PML_RES
// ---- the Wi-Fi files --------------------------------------------------------------------------
static bool s_res_scan; // installing for a network scan (F1), not for the web server (F4)

static void draw_res(void)
{
    draw_dialog(128);
    int y = LIST_Y + 4;
    lcd_text(MARGIN, y, "Install the Wi-Fi files?", C_WARN, FONT_LARGE);
    draw_wrapped(y + 2 * ROW_H, "Wi-Fi needs the last 256 KB of flash. The installed app keeps its data there: "
                 "installing erases it.", C_FG);
    draw_footer(NULL, C_FG, "Enter Install  Esc Back");
}

static void draw_progress(size_t done, size_t total);

// Copies the Wi-Fi files from pml2350.uf2 into flash. On failure, shows the error screen.
static bool res_install(void)
{
    char err[80];
    draw_header();
    lcd_text(MARGIN, LIST_Y + 4, "Installing the Wi-Fi files...", C_ACCENT, FONT_LARGE);
    draw_footer(NULL, C_FG, "Do not switch off");
    lcd_flush();
    draw_progress(0, 1);
    if (!uf2_res_install(draw_progress, err, sizeof(err))) {
        snprintf(s_error, sizeof(s_error), "Could not install the Wi-Fi files: %s", err);
        strlcpy(s_sel_name, "(Wi-Fi files)", sizeof(s_sel_name));
        s_screen = SCREEN_ERROR;
        return false;
    }
    return true;
}

// True if Wi-Fi can start. If not, installs the files when nothing is lost, or asks first.
static bool res_ensure(bool for_scan)
{
    switch (uf2_res_check()) {
    case RES_OK:
        return true;
    case RES_BLANK:
    case RES_OLD:
        return res_install();
    case RES_USED:
        s_res_scan = for_scan;
        s_screen = SCREEN_RES;
        return false;
    default:
        snprintf(s_error, sizeof(s_error), "This pml2350.uf2 was built without the Wi-Fi files. "
                 "Copy a release build to the SD card.");
        strlcpy(s_sel_name, "(Wi-Fi files)", sizeof(s_sel_name));
        s_screen = SCREEN_ERROR;
        return false;
    }
}

static void wifi_scan(void);
static void enter_web(void);

static bool res_key(int key)
{
    if (key == KEY_ENTER) {
        if (res_install()) {
            if (s_res_scan) {
                s_screen = SCREEN_WIFI;
                wifi_scan();
            } else {
                enter_web();
            }
        }
        return true;
    }
    if (key == KEY_ESC || key == KEY_BACKSPACE || key == KEY_LEFT) {
        s_screen = s_res_scan ? SCREEN_WIFI : s_base;
        return true;
    }
    return false;
}
#endif

// Start the web server in the background. False if it can't (an error or install screen is shown).
static bool web_start(void)
{
    if (!s_sd_ok) {
        snprintf(s_error, sizeof(s_error), "Insert an SD card first.");
        strlcpy(s_sel_name, "(SD card)", sizeof(s_sel_name));
        s_screen = SCREEN_ERROR;
        return false;
    }
#if PML_RES
    if (!res_ensure(false)) {
        return false;
    }
#endif
    s_web_changes_seen = webserver_changes();
    if (!webserver_start()) {
        snprintf(s_error, sizeof(s_error), "Could not start the web server: the Wi-Fi chip did not start.");
        strlcpy(s_sel_name, "(web server)", sizeof(s_sel_name));
        s_screen = SCREEN_ERROR;
        return false;
    }
    return true;
}

static void enter_web(void)
{
    if (web_start()) {
        s_screen = SCREEN_WEB;
    }
}
#endif

// ---- Setup (F1 > Setup) -------------------------------------------------------------------------------
#if PICOMULTILOADER_WIFI
enum { WF_SSID, WF_PASS, WF_SHOW, WF_SCAN, WF_SAVE, WF_FORGET, WF_AUTO, WF_START, WF_TZ, WF_HELP, WF_ROWS };
#else
enum { WF_START, WF_HELP, WF_ROWS };
#endif
#define WF_ROW_H   17
#define WF_FIELD_X (MARGIN + 74)
#define WF_COLS    ((LCD_W - MARGIN - WF_FIELD_X - 4) / 7)
#define MAX_APS    16

static int s_wf_row;
static char s_wf_msg[64];
static lcd_color_t s_wf_msg_color;
#if PICOMULTILOADER_WIFI
static char s_wf_ssid[33], s_wf_pass[65];
static bool s_wf_show, s_wf_dirty;
static bool s_web_auto; // start the web server at switch-on
static web_ap_t s_aps[MAX_APS];
static int s_ap_count, s_ap_sel, s_ap_top;
#endif

static void wf_message(const char *msg, lcd_color_t color)
{
    strlcpy(s_wf_msg, msg, sizeof(s_wf_msg));
    s_wf_msg_color = color;
}

// ---- help (F1 > Help) -------------------------------------------------------------------------
// "#" starts a heading, "" is a blank line; everything else is wrapped to the screen width.
static const char *const HELP_TEXT[] = {
    "#What it does",
#if PICO_RP2040
    "Copy Raspberry Pi Pico (RP2040) programs (.uf2 files) to the SD card, pick one here, and the loader flashes it and boots it.",
#else
    "Copy Raspberry Pi Pico 2 (RP2350) programs (.uf2 files) to the SD card, pick one here, and the loader flashes it and boots it.",
#endif
    "",
    "#Home",
    "The apps on the card: each folder in the root with an app.ini or a .uf2 in it, and each .uf2 in the root.",
    "Arrows, PgUp/PgDn, Home/End: move. Tab: next tab (All, Recent, Favourites, then by category).",
    "Type to search all the apps; Backspace deletes, Esc clears.",
    "Enter: run the app. The installed one (green dot) starts at once; any other is flashed first.",
    "F2: add to or remove from Favourites (yellow star). F3: Files.",
#if PICOMULTILOADER_WIFI
    "F1: control center (Wi-Fi, USB drive, backlight, theme, Setup and this help). F4: web file manager. F5: terminal.",
#else
    "F1: control center (USB drive, backlight, theme, Setup and this help). F5: terminal.",
#endif
    "No card? Insert a FAT32 card and press Enter.",
    "",
    "#App folders",
    "An app.ini in the folder describes the app, one key = value per line: name, category (Emulators, Tools, Dev "
    "or Media), icon (a 32x32 16-colour BMP, icon.bmp if not set), binary (the .uf2 to run), version, author, desc "
    "and board (rp2040, rp2350 or any). A folder with several .uf2 files and no binary opens in Files.",
    "",
    "#Files (F3)",
    "Up/Down, PgUp/PgDn, Home/End: move. Enter or Right: open a folder, or show a .uf2 file's details; Enter there "
    "flashes and runs it (or just runs it if it is installed; R reflashes).",
    "Backspace or Left: parent folder; at the top, back to Home. Esc or F3: Home. F2: favourite.",
    "Folders, .uf2 files and files the viewers open are listed (those in another colour). The last folder and file are remembered.",
    "",
    "#Text reader and image viewer",
    "Enter on a .txt, .md, .log, .ini, .csv, .py or .bas file reads it in pages: Up/Down or PgUp/PgDn turn them, "
    "Home/End go to the first and last. Markdown headings are shown in colour.",
    "Enter on a .bmp shows it on the whole screen, scaled to fit (uncompressed 1, 4, 8, 16, 24 or 32 bits). "
    "Pictures with up to 16 colours are shown as they are; others are dithered. G: grey. Left/Right: the previous "
    "or next picture in the folder. Esc: back.",
    "In the terminal, view <file> opens them too.",
    "",
    "#Copy, move and delete (Tab)",
    "Tab opens a menu for the selected file or folder: Copy, Cut (to move it), Paste and Delete.",
    "Shortcuts in the file list: C copy, X cut, V paste into the folder shown, Del delete. Esc forgets what was copied.",
    "Copy or Cut, go to another folder, then Paste. Pasting a copy where the name is taken adds a number, e.g. app (2).uf2.",
    "Deleting a folder deletes everything in it.",
#if !PICO_RP2040
    "pml2350.uf2 in the root (this menu) can't be deleted or moved from here.",
#endif
    "",
    "#Installing the loader",
#if PICO_RP2040
    "PicoMultiLoader is the Pico's own firmware, no other bootloader needed: hold BOOTSEL, plug in USB and copy "
    "PicoMultiLoader-rp2040.uf2 to the drive that appears.",
#else
    "PicoMultiLoader replaces uf2loader: hold BOOTSEL, plug in USB and copy PicoMultiLoader-rp2350.uf2 (an 8 KB boot "
    "stub) to the drive that appears. Put pml2350.uf2 in the root of the SD card: the stub loads this menu from it.",
    "Without that file the menu can't open; the stub then starts the app, or BOOTSEL if there is none.",
#endif
    "",
    "#Back to the loader",
    "Hold Up, F1 or F5 while switching on: this menu opens instead of the app. "
    "It also opens when no app is installed, and when an app asks for it (the uf2loader way).",
    "",
    "#Which files work",
#if PICO_RP2040
    "UF2 files for the RP2040 built to run from flash (the normal Pico SDK build), up to 1788 KB.",
#else
    "UF2 files for the RP2350 built to run from flash (the normal Pico SDK build), up to 4088 KB. RP2040 files are refused.",
#endif
    "RAM-only (no_flash) apps are not supported. The installed app keeps working after a restart.",
    "Each file is checked before flashing; an unsuitable one shows why.",
    "",
    "#Control center (F1)",
#if PICOMULTILOADER_WIFI
    "Wi-Fi: Enter turns Wi-Fi and the web server on or off; the address is shown below it.",
#endif
    "USB drive: Enter starts USB drive mode. Backlight: Left/Right. Theme: Left/Right or Enter; your own .pal files go in " THEME_DIR ".",
    "Idle screen: after that long without a key, a dim clock on black with the backlight turned down, to save the battery; "
    "10 minutes later the backlight goes off. Any key wakes it up, and does nothing else. Until the time is known it "
    "shows how long the loader has been on.",
    "Setup: the settings below. The battery charge is at the bottom. Esc or F1 closes it.",
    "",
#if PICOMULTILOADER_WIFI
    "#Date and time",
    "There is no clock chip: while Wi-Fi is on and joined to your network, the loader asks ip-api.com for the time and "
    "the time zone of your connection (summer time included), then every 6 hours. It is lost when an app runs or the power goes off.",
    "It shows in the middle of the status bar and on the idle screen. Terminal: date shows it. tz +01:00 sets the time zone by hand; tz auto goes back to the one from the web.",
    "Files the loader writes (uploads, screenshots) get the date once it is known.",
    "",
#endif
    "#Screenshots (F10)",
    "Shift+F5 (F10) saves the screen as a picture in " SHOT_DIR " on the SD card: shot-001.bmp, shot-002.bmp, ...",
    "",
    "#Messages from apps",
    "An app can leave a message for the loader: the first line of " PML_DIR "/exit.txt is shown when the menu opens, then the "
    "file is deleted. Or set watchdog scratch[2] to 0x504D4C58 and scratch[3] to an exit code before rebooting.",
    "",
    "#Setup (F1 > Setup)",
#if PICOMULTILOADER_WIFI
    "Type the network name and password, or choose Scan for networks and pick one.",
    "Up/Down or Tab: move. Backspace: delete. Enter: select.",
    "Save keeps the network in picomultiloader.cfg on the SD card. Forget removes it.",
    "2.4 GHz networks only. Passwords are 8-64 characters, or empty for an open network.",
    "Wi-Fi autostart: ON starts the web file manager in the background at switch-on, as if you pressed F4 and then Esc. OFF (the default) waits for F4.",
#else
    "Up/Down: move. Enter: select. Settings are kept in picomultiloader.cfg on the SD card.",
#endif
    "Start folder: the folder Files (F3) opens in at switch-on. Choose one, or Remember the last folder (the default).",
#if PICOMULTILOADER_WIFI
    "Time zone: Left/Right. auto (the default) takes it from the web with the time; or pick an offset from UTC (no summer time change then).",
#endif
    "",
#if PICOMULTILOADER_WIFI
    "#Web file manager (F4)",
    "Joins your saved network and shows the address to open in a browser. http://" WEB_HOSTNAME ".local/ often works too.",
    "With no network saved, or if joining fails within 20 s, it makes its own network PicoMultiLoader-XXXX. Its password and the address http://192.168.4.1/ are shown on screen.",
    "In the browser: upload (drag and drop works), download, rename, delete, new folder, and Flash a .uf2 straight away.",
    "There is no login: use it on trusted networks only.",
    "Esc goes back and keeps the server running. A Wi-Fi icon at the top shows it is on: green ready, blue starting, amber reconnecting, red failed. Home and Files refresh when the browser changes files.",
    "F4 shows the server screen again; F4 there stops it. Flashing a file or USB drive mode stops it too.",
    "",
#endif
    "#Terminal (F5)",
    "Type commands, Enter runs them: help lists them all. ls, cd, cat and pwd work on the SD card; run starts an app or a .uf2; "
    "apps, recent, fav, theme, date, tz, battery, free, df, about and more.",
#if PICOMULTILOADER_WIFI
    "wifi on / wifi off start and stop the web server; ip shows its address.",
#endif
    "Tab completes a command or a file name. Up/Down: earlier commands. PgUp/PgDn: scroll. Esc clears the line; "
    "Esc on an empty line, F5 or exit closes the terminal.",
    "Everything it prints also goes to the serial console, and you can type the commands there too.",
    "",
    "#USB drive mode (F1 > USB drive)",
    "Shows the SD card on a PC as a USB drive. Needs a cable on the Pico's micro-USB port; the PicoCalc's USB-C port is serial only.",
    "Eject the drive on the PC when done: the loader takes the card back by itself. Esc also leaves, but only press it after ejecting.",
#if PICOMULTILOADER_WIFI
    "Starting USB drive mode stops the web server.",
#endif
    "",
    "#Serial console",
    "115200 baud on the PicoCalc's USB-C port. Arrows, Enter, Tab, Backspace, Del, Esc, F1-F5 and typing work like the PicoCalc keyboard. Ctrl+P dumps the screen.",
    "",
    "#This help",
    "Up/Down: scroll. PgUp/PgDn: page. Home/End: top/bottom. Esc: back.",
};

static int s_help_count, s_help_top;

// Wrap HELP_TEXT into screen lines on the fly, drawing lines first..first+rows-1 at the top of the
// list. Returns how many lines there are.
static int help_lines(int first, int rows)
{
    int k = 0;
    for (size_t i = 0; i < sizeof(HELP_TEXT) / sizeof(HELP_TEXT[0]); i++) {
        const char *t = HELP_TEXT[i];
        bool heading = t[0] == '#';
        t += heading;
        size_t len = strlen(t);
        if (!len) {
            k++;
        }
        for (size_t pos = 0; pos < len; k++) {
            size_t n = len - pos > COLS - 1 ? COLS - 1 : len - pos; // leave room for the scroll bar
            if (pos + n < len) { // break at the last space
                size_t sp = n;
                while (sp > 0 && t[pos + sp] != ' ') {
                    sp--;
                }
                if (sp > 0) {
                    n = sp;
                }
            }
            if (k >= first && k < first + rows) {
                char line[COLS + 1];
                memcpy(line, t + pos, n);
                line[n] = '\0';
                lcd_text(MARGIN, LIST_Y + (k - first) * ROW_H, line, heading ? C_ACCENT : C_FG, FONT_LARGE);
            }
            pos += n;
            while (t[pos] == ' ') {
                pos++;
            }
        }
    }
    return k;
}

static void enter_help(void)
{
    if (!s_help_count) {
        s_help_count = help_lines(0, 0);
    }
    s_help_top = 0;
    s_screen = SCREEN_HELP;
}

static void draw_help(void)
{
    draw_header();
    int max_top = s_help_count > LIST_ROWS ? s_help_count - LIST_ROWS : 0;
    if (s_help_top > max_top) {
        s_help_top = max_top;
    }
    help_lines(s_help_top, LIST_ROWS);
    if (max_top) { // scroll bar
        int track = LIST_ROWS * ROW_H;
        int h = track * LIST_ROWS / s_help_count;
        int y = LIST_Y + (track - h) * s_help_top / max_top;
        lcd_fill_rect(LCD_W - 3, y - 1, 2, h, C_DIM);
    }
    char where[32];
    int last = s_help_top + LIST_ROWS < s_help_count ? s_help_top + LIST_ROWS : s_help_count;
    snprintf(where, sizeof(where), "Help  line %d of %d", last, s_help_count);
    draw_footer(where, C_DIM, "Up/Down Scroll  PgUp/PgDn Page  Esc Back");
}

static bool help_key(int key)
{
    switch (key) {
    case KEY_UP:
        s_help_top -= s_help_top > 0;
        return true;
    case KEY_DOWN:
        s_help_top++; // clamped when drawing
        return true;
    case KEY_PAGE_UP:
    case KEY_LEFT:
        s_help_top = s_help_top > LIST_ROWS - 1 ? s_help_top - (LIST_ROWS - 1) : 0;
        return true;
    case KEY_PAGE_DOWN:
    case KEY_RIGHT:
    case ' ':
        s_help_top += LIST_ROWS - 1;
        return true;
    case KEY_HOME:
        s_help_top = 0;
        return true;
    case KEY_END:
        s_help_top = s_help_count;
        return true;
    case KEY_ESC:
    case KEY_ENTER:
    case KEY_BACKSPACE:
    case KEY_F1:
        s_screen = SCREEN_WIFI;
        return true;
    default:
        return false;
    }
}

static void enter_wifi(void)
{
#if PICOMULTILOADER_WIFI
    if (!s_sd_ok || !web_wifi_load(s_wf_ssid, sizeof(s_wf_ssid), s_wf_pass, sizeof(s_wf_pass))) {
        s_wf_ssid[0] = s_wf_pass[0] = '\0';
    }
    s_wf_show = false;
    s_wf_dirty = false;
    s_wf_row = s_wf_ssid[0] ? WF_PASS : WF_SSID;
    wf_message(s_wf_ssid[0] ? "This network is used when you press F4." : "No network set up yet.", C_DIM);
#else
    s_wf_row = WF_START;
    wf_message("", C_DIM);
#endif
    s_screen = SCREEN_WIFI;
}

// ---- start folder (Setup) ---------------------------------------------------------------------
// Rows: "use this folder", "remember the last folder", then ".." (below the root) and subfolders.
enum { PICK_USE, PICK_LAST, PICK_FIRST };
#define MAX_PICK 64

static char s_start_dir[PATH_MAX_LEN]; // folder to open at switch-on, "/" = root; "" = the last one used
static char s_pick[PATH_MAX_LEN];      // folder being looked at in the picker ("" = root)
static char *s_pick_dirs[MAX_PICK];
static int s_pick_count, s_pick_sel, s_pick_top;

static bool pick_at_root(void)
{
    return strcmp(s_pick, SDCARD_MOUNT_POINT) == 0;
}

static int pick_rows(void)
{
    return PICK_FIRST + (pick_at_root() ? 0 : 1) + s_pick_count;
}

// Subfolder index for a row, -1 for "..", -2 for the two fixed rows.
static int pick_dir_index(int row)
{
    if (row < PICK_FIRST) {
        return -2;
    }
    row -= PICK_FIRST;
    if (!pick_at_root()) {
        if (row == 0) {
            return -1;
        }
        row--;
    }
    return row;
}

static int cmp_names(const void *a, const void *b)
{
    return strcasecmp(*(char *const *)a, *(char *const *)b);
}

static void pick_scan(void)
{
    for (int i = 0; i < s_pick_count; i++) {
        free(s_pick_dirs[i]);
    }
    s_pick_count = 0;
    s_pick_sel = s_pick_top = 0;
    DIR dir;
    if (f_opendir(&dir, sd_path(s_pick)) != FR_OK) {
        return;
    }
    FILINFO fi;
    while (s_pick_count < MAX_PICK && f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
        if (!(fi.fattrib & AM_DIR) || hidden(&fi)) {
            continue;
        }
        char *name = strdup(fi.fname);
        if (name) {
            s_pick_dirs[s_pick_count++] = name;
        }
    }
    f_closedir(&dir);
    qsort(s_pick_dirs, s_pick_count, sizeof(s_pick_dirs[0]), cmp_names);
}

// "SD/apps" for "/apps", "SD/" for the root.
static void sd_name(char *out, size_t len, const char *path)
{
    snprintf(out, len, "SD%s", path[0] ? path : "/");
}

static void enter_pick(void)
{
    if (!s_sd_ok) {
        wf_message("Insert an SD card first.", C_ERR);
        return;
    }
    const char *from = s_start_dir[0] ? s_start_dir : s_cwd;
    copy_dir(s_pick, sizeof(s_pick), is_dir(from) ? from : SDCARD_MOUNT_POINT);
    pick_scan();
    s_screen = SCREEN_PICK;
}

static void draw_pick(void)
{
    s_path_override = s_pick;
    draw_header();
    s_path_override = NULL;
    int n = pick_rows();
    if (s_pick_sel < s_pick_top) {
        s_pick_top = s_pick_sel;
    } else if (s_pick_sel >= s_pick_top + LIST_ROWS) {
        s_pick_top = s_pick_sel - LIST_ROWS + 1;
    }
    for (int r = 0; r < LIST_ROWS && s_pick_top + r < n; r++) {
        int row = s_pick_top + r, y = LIST_Y + r * ROW_H;
        bool sel = row == s_pick_sel;
        if (sel) {
            lcd_fill_rect(MARGIN - 4, y - 1, LCD_W - 2 * (MARGIN - 4), ROW_H, C_SEL_BG);
        }
        char label[COLS + 1], buf[PATH_MAX_LEN + 8];
        int d = pick_dir_index(row);
        lcd_color_t fg = sel ? C_SEL_FG : C_FG;
        if (row == PICK_USE) {
            fit(label, "Use this folder", COLS);
            fg = sel ? C_SEL_FG : C_OK;
        } else if (row == PICK_LAST) {
            fit(label, "Remember the last folder instead", COLS);
            fg = sel ? C_SEL_FG : C_ACCENT;
        } else if (d == -1) {
            fit(label, "[..]", COLS);
        } else {
            snprintf(buf, sizeof(buf), "[%s]", s_pick_dirs[d]);
            fit(label, buf, COLS);
        }
        lcd_text(MARGIN, y, label, fg, FONT_LARGE);
    }
    if (n > LIST_ROWS) { // scroll bar
        int track = LIST_ROWS * ROW_H;
        int h = track * LIST_ROWS / n;
        int y = LIST_Y + (track - h) * s_pick_top / (n - LIST_ROWS);
        lcd_fill_rect(LCD_W - 3, y - 1, 2, h, C_DIM);
    }
    draw_footer("Choose the folder to open at switch-on", C_DIM, "Enter Open/choose  Left Up  Esc Cancel");
}

static void pick_up(void)
{
    char *slash = strrchr(s_pick, '/');
    if (pick_at_root() || !slash) {
        return;
    }
    char prev[64];
    strlcpy(prev, slash + 1, sizeof(prev));
    *slash = '\0';
    pick_scan();
    for (int row = PICK_FIRST; row < pick_rows(); row++) { // select the folder we came from
        int d = pick_dir_index(row);
        if (d >= 0 && strcmp(s_pick_dirs[d], prev) == 0) {
            s_pick_sel = row;
            break;
        }
    }
}

static bool pick_key(int key)
{
    int n = pick_rows();
    switch (key) {
    case KEY_UP:
        s_pick_sel = s_pick_sel > 0 ? s_pick_sel - 1 : n - 1;
        return true;
    case KEY_DOWN:
        s_pick_sel = s_pick_sel + 1 < n ? s_pick_sel + 1 : 0;
        return true;
    case KEY_PAGE_UP:
        s_pick_sel = s_pick_sel > LIST_ROWS ? s_pick_sel - LIST_ROWS : 0;
        return true;
    case KEY_PAGE_DOWN:
        s_pick_sel = s_pick_sel + LIST_ROWS < n ? s_pick_sel + LIST_ROWS : n - 1;
        return true;
    case KEY_HOME:
        s_pick_sel = 0;
        return true;
    case KEY_END:
        s_pick_sel = n - 1;
        return true;
    case KEY_BACKSPACE:
    case KEY_LEFT:
        pick_up();
        return true;
    case KEY_ESC:
        s_screen = SCREEN_WIFI;
        return true;
    case KEY_ENTER:
    case KEY_RIGHT:
        break;
    default:
        return false;
    }
    int d = pick_dir_index(s_pick_sel);
    if (s_pick_sel == PICK_USE || s_pick_sel == PICK_LAST) {
        if (key == KEY_RIGHT) {
            return true;
        }
        char msg[64], name[48];
        if (s_pick_sel == PICK_USE) {
            strlcpy(s_start_dir, sd_path(s_pick), sizeof(s_start_dir));
            sd_name(name, sizeof(name), s_pick);
            snprintf(msg, sizeof(msg), "Files opens %.36s at switch-on.", name);
        } else {
            s_start_dir[0] = '\0';
            snprintf(msg, sizeof(msg), "Opens the last folder used.");
        }
        save_setting("start_dir", s_start_dir);
        printf("ui: start folder %s\n", s_start_dir[0] ? s_start_dir : "(last used)");
        wf_message(msg, C_OK);
        s_screen = SCREEN_WIFI;
    } else if (d == -1) {
        pick_up();
    } else if (strlen(s_pick) + 1 + strlen(s_pick_dirs[d]) < sizeof(s_pick)) {
        strlcat(s_pick, "/", sizeof(s_pick));
        strlcat(s_pick, s_pick_dirs[d], sizeof(s_pick));
        pick_scan();
    }
    return true;
}

static void draw_option(int y, const char *text, bool sel)
{
    if (sel) {
        lcd_fill_rect(MARGIN - 4, y - 3, LCD_W - 2 * (MARGIN - 4), ROW_H + 2, C_SEL_BG);
    }
    lcd_text(MARGIN, y, text, sel ? C_SEL_FG : C_FG, FONT_LARGE);
}

static void draw_start_option(int y)
{
    char start[COLS + 1], buf[PATH_MAX_LEN + 24], name[PATH_MAX_LEN + 4];
    if (s_start_dir[0]) {
        sd_name(name, sizeof(name), strcmp(s_start_dir, "/") == 0 ? "" : s_start_dir);
    } else {
        strlcpy(name, "last used", sizeof(name));
    }
    snprintf(buf, sizeof(buf), "Start folder: %s", name);
    fit(start, buf, COLS);
    draw_option(y, start, s_wf_row == WF_START);
}

#if PICOMULTILOADER_WIFI
// Time zone choices: auto (from the web), then the UTC offsets in use
static const char *const TZ_OPTS[] = {
    "auto", "-12:00", "-11:00", "-10:00", "-09:30", "-09:00", "-08:00", "-07:00", "-06:00", "-05:00", "-04:00",
    "-03:30", "-03:00", "-02:00", "-01:00", "+00:00", "+01:00", "+02:00", "+03:00", "+03:30", "+04:00", "+04:30",
    "+05:00", "+05:30", "+05:45", "+06:00", "+06:30", "+07:00", "+08:00", "+08:45", "+09:00", "+09:30", "+10:00",
    "+10:30", "+11:00", "+12:00", "+12:45", "+13:00", "+14:00",
};
#define TZ_COUNT ((int)(sizeof(TZ_OPTS) / sizeof(TZ_OPTS[0])))

static int tz_opt(void)
{
    char v[16];
    if (load_setting("tz", v, sizeof(v)) && v[0]) {
        for (int i = 1; i < TZ_COUNT; i++) {
            if (strcmp(v, TZ_OPTS[i]) == 0) {
                return i;
            }
        }
    }
    return 0;
}

static void set_tz(int step)
{
    int i = (tz_opt() + step + TZ_COUNT) % TZ_COUNT;
    clock_set_setting(TZ_OPTS[i]);
}

static void draw_tz_option(int y)
{
    char text[COLS + 1], buf[80];
    if (tz_opt() == 0) {
        snprintf(buf, sizeof(buf), "Time zone: < auto%s%s%s >", clock_known() ? " (" : "",
                 clock_known() ? clock_zone_name() : "", clock_known() ? ")" : "");
    } else {
        snprintf(buf, sizeof(buf), "Time zone: < %s >", clock_zone_name());
    }
    fit(text, buf, COLS);
    draw_option(y, text, s_wf_row == WF_TZ);
}

static void draw_field(int y, const char *label, const char *text, bool masked, bool sel)
{
    lcd_text(MARGIN, y, label, sel ? C_ACCENT : C_DIM, FONT_LARGE);
    lcd_fill_rect(WF_FIELD_X - 4, y - 3, LCD_W - MARGIN - WF_FIELD_X + 4, ROW_H + 2, sel ? C_SEL_BG : C_FIELD);
    char shown[66];
    size_t len = strlen(text);
    if (masked) {
        memset(shown, '*', len);
        shown[len] = '\0';
    } else {
        strlcpy(shown, text, sizeof(shown));
    }
    if (sel) {
        strlcat(shown, "_", sizeof(shown)); // cursor
    }
    size_t n = strlen(shown);
    const char *tail = n > WF_COLS ? shown + n - WF_COLS : shown; // keep the end (where you type) visible
    lcd_text(WF_FIELD_X, y, tail, sel ? C_SEL_FG : C_FG, FONT_LARGE);
}

static void draw_wifi(void)
{
    draw_header();
    int y = LIST_Y + 4;
    int x = lcd_text(MARGIN, y, "Setup", C_ACCENT, FONT_LARGE);
    lcd_text(x + 10, y, "Wi-Fi for the web server (F4)", C_DIM, FONT_LARGE);
    y += WF_ROW_H + 3;
    draw_field(y, "Network", s_wf_ssid, false, s_wf_row == WF_SSID);
    y += WF_ROW_H;
    draw_field(y, "Password", s_wf_pass, !s_wf_show, s_wf_row == WF_PASS);
    y += WF_ROW_H + 3;
    draw_option(y, s_wf_show ? "Show password: yes" : "Show password: no", s_wf_row == WF_SHOW);
    y += WF_ROW_H;
    draw_option(y, "Scan for networks...", s_wf_row == WF_SCAN);
    y += WF_ROW_H;
    draw_option(y, s_wf_dirty ? "Save *" : "Save", s_wf_row == WF_SAVE);
    y += WF_ROW_H;
    draw_option(y, "Forget this network", s_wf_row == WF_FORGET);
    y += WF_ROW_H + 3;
    draw_option(y, s_web_auto ? "Wi-Fi autostart: ON" : "Wi-Fi autostart: OFF", s_wf_row == WF_AUTO);
    y += WF_ROW_H;
    draw_start_option(y);
    y += WF_ROW_H;
    draw_tz_option(y);
    y += WF_ROW_H;
    draw_option(y, "Help: how to use the loader", s_wf_row == WF_HELP);
    y += WF_ROW_H + 4;
    if (s_wf_msg[0]) { // the last result, else a hint for the row (no room for both)
        lcd_text(MARGIN, y, s_wf_msg, s_wf_msg_color, FONT_LARGE);
    } else {
        draw_wrapped(y, s_wf_row <= WF_PASS    ? "Type to edit, Backspace deletes. 2.4 GHz networks only."
                        : s_wf_row == WF_HELP ? "Keys, flashing, Wi-Fi, web and USB drive mode explained."
                        : s_wf_row == WF_START ? "The folder Files (F3) opens in when you switch on."
                        : s_wf_row == WF_TZ    ? "Left/Right: auto takes it from the web (summer time included), or pick an offset from UTC."
                        : s_wf_row == WF_AUTO  ? "ON: the web file manager starts by itself at switch-on."
                                              : "F4 joins this network and starts the web file manager.",
                     C_DIM);
    }
    draw_footer(NULL, C_FG, s_wf_dirty ? "Up/Down Move  Enter Select  Esc Discard"
                                       : "Up/Down Move  Enter Select  Esc Back");
}

static void draw_signal(int x, int y, int rssi, lcd_color_t color)
{
    int bars = rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -78 ? 2 : 1;
    for (int i = 0; i < 4; i++) {
        int h = 3 + 3 * i;
        lcd_fill_rect(x + i * 4, y + 12 - h, 3, h, i < bars ? color : C_SIG_OFF);
    }
}

static void draw_wifi_scan(void)
{
    draw_header();
    lcd_text(MARGIN, LIST_Y - 2, s_ap_count ? "Choose a network" : "No networks found", C_ACCENT, FONT_LARGE);
    int top = LIST_Y + ROW_H + 2, rows = (FOOTER_Y - 4 - top) / ROW_H;
    if (s_ap_sel < s_ap_top) {
        s_ap_top = s_ap_sel;
    } else if (s_ap_sel >= s_ap_top + rows) {
        s_ap_top = s_ap_sel - rows + 1;
    }
    for (int r = 0; r < rows && s_ap_top + r < s_ap_count; r++) {
        int i = s_ap_top + r, y = top + r * ROW_H;
        bool sel = i == s_ap_sel;
        if (sel) {
            lcd_fill_rect(MARGIN - 4, y - 1, LCD_W - 2 * (MARGIN - 4), ROW_H, C_SEL_BG);
        }
        char name[COLS + 1];
        fit(name, s_aps[i].ssid, COLS - 6);
        lcd_text(MARGIN, y, name, sel ? C_SEL_FG : C_FG, FONT_LARGE);
        if (!s_aps[i].secure) {
            lcd_text(LCD_W - MARGIN - 18 - 7 * 5, y, "open", sel ? C_SEL_FG : C_DIM, FONT_LARGE);
        }
        draw_signal(LCD_W - MARGIN - 16, y, s_aps[i].rssi, sel ? C_SEL_FG : C_OK);
    }
    draw_footer(NULL, C_FG, s_ap_count ? "Up/Down Select  Enter Choose  Esc Back" : "Esc Back");
}

static void wifi_scan(void)
{
    if (web_running()) {
        wf_message("Stop the web server (F4) to scan.", C_ERR);
        return;
    }
#if PML_RES
    if (!res_ensure(true)) {
        return;
    }
    s_screen = SCREEN_WIFI; // an install may have shown its progress
#endif
    wf_message("Scanning...", C_FG);
    draw_wifi();
    lcd_flush();
    int n = web_wifi_scan(s_aps, MAX_APS);
    if (n < 0) {
        wf_message("Scan failed - is the Wi-Fi chip working?", C_ERR);
        return;
    }
    s_ap_count = n;
    s_ap_sel = s_ap_top = 0;
    s_screen = SCREEN_WIFI_SCAN;
}

static void wifi_save(void)
{
    size_t pl = strlen(s_wf_pass);
    if (!s_wf_ssid[0]) {
        wf_message("Enter the network name first.", C_ERR);
        s_wf_row = WF_SSID;
    } else if (pl > 0 && pl < 8) {
        wf_message("The password needs 8+ characters.", C_ERR);
        s_wf_row = WF_PASS;
    } else if (!s_sd_ok || !web_wifi_save(s_wf_ssid, s_wf_pass)) {
        wf_message("Could not save (SD card?).", C_ERR);
    } else {
        s_wf_dirty = false;
        wf_message(web_running() ? "Saved. Restart the web server to use it." : "Saved. Press F4 to connect.", C_OK);
    }
}
#else
static void draw_wifi(void)
{
    draw_header();
    int y = LIST_Y + 4;
    lcd_text(MARGIN, y, "Setup", C_ACCENT, FONT_LARGE);
    y += WF_ROW_H + 3;
    draw_start_option(y);
    y += WF_ROW_H;
    draw_option(y, "Help: how to use the loader", s_wf_row == WF_HELP);
    y += WF_ROW_H + 4;
    lcd_text(MARGIN, y, s_wf_msg, s_wf_msg_color, FONT_LARGE);
    y += ROW_H + 2;
    draw_wrapped(y, s_wf_row == WF_HELP    ? "Keys, flashing and USB drive mode explained."
                                           : "The folder Files (F3) opens in when you switch on.",
                 C_DIM);
    draw_footer(NULL, C_FG, "Up/Down Move  Enter Select  Esc Back");
}
#endif

// Returns false if the key isn't used here.
static bool wifi_key(int key)
{
#if PICOMULTILOADER_WIFI
    char *field = s_wf_row == WF_SSID ? s_wf_ssid : s_wf_row == WF_PASS ? s_wf_pass : NULL;
    size_t cap = s_wf_row == WF_SSID ? sizeof(s_wf_ssid) : sizeof(s_wf_pass);
    if (field && key >= 0x20 && key < 0x7F) {
        size_t len = strlen(field);
        if (len + 1 < cap) {
            field[len] = (char)key;
            field[len + 1] = '\0';
            s_wf_dirty = true;
        }
        return true;
    }
#endif
    switch (key) {
    case KEY_UP:
        s_wf_row = (s_wf_row + WF_ROWS - 1) % WF_ROWS;
        s_wf_msg[0] = '\0';
        return true;
    case KEY_DOWN:
    case KEY_TAB:
        s_wf_row = (s_wf_row + 1) % WF_ROWS;
        s_wf_msg[0] = '\0';
        return true;
#if PICOMULTILOADER_WIFI
    case KEY_LEFT:
    case KEY_RIGHT:
        if (s_wf_row != WF_TZ) {
            return false;
        }
        set_tz(key == KEY_RIGHT ? 1 : -1);
        return true;
#endif
    case KEY_HOME:
        s_wf_row = 0;
        return true;
    case KEY_END:
        s_wf_row = WF_ROWS - 1; // Help
        return true;
#if PICOMULTILOADER_WIFI
    case KEY_BACKSPACE:
    case KEY_DEL:
        if (field && field[0]) {
            field[strlen(field) - 1] = '\0';
            s_wf_dirty = true;
        }
        return true;
#endif
    case KEY_ESC:
        s_screen = SCREEN_CTL;
        return true;
    case KEY_F1:
        s_screen = s_base;
        return true;
    case KEY_ENTER:
        break;
    default:
        return false;
    }
    switch (s_wf_row) {
#if PICOMULTILOADER_WIFI
    case WF_SSID:
        s_wf_row = WF_PASS;
        break;
    case WF_PASS:
        s_wf_row = WF_SAVE;
        break;
    case WF_SHOW:
        s_wf_show = !s_wf_show;
        break;
    case WF_SCAN:
        wifi_scan();
        break;
    case WF_SAVE:
        wifi_save();
        break;
    case WF_FORGET:
        web_wifi_save("", "");
        s_wf_ssid[0] = s_wf_pass[0] = '\0';
        s_wf_dirty = false;
        s_wf_row = WF_SSID;
        wf_message(web_running() ? "Forgotten. Used when the server restarts." : "Forgotten. F4 will start its own network.", C_OK);
        break;
    case WF_AUTO:
        s_web_auto = !s_web_auto;
        save_setting("web_auto", s_web_auto ? "1" : "0");
        printf("ui: Wi-Fi autostart %s\n", s_web_auto ? "on" : "off");
        wf_message(s_web_auto ? "The web server starts at switch-on." : "The web server waits for F4.", C_OK);
        break;
#endif
    case WF_START:
        enter_pick();
        break;
#if PICOMULTILOADER_WIFI
    case WF_TZ:
        set_tz(1);
        break;
#endif
    case WF_HELP:
        enter_help();
        break;
    }
    return true;
}

#if PICOMULTILOADER_WIFI
static bool wifi_scan_key(int key)
{
    switch (key) {
    case KEY_UP:
        s_ap_sel = s_ap_sel > 0 ? s_ap_sel - 1 : (s_ap_count ? s_ap_count - 1 : 0);
        return true;
    case KEY_DOWN:
        s_ap_sel = s_ap_sel + 1 < s_ap_count ? s_ap_sel + 1 : 0;
        return true;
    case KEY_ENTER:
        if (s_ap_count) {
            web_ap_t *ap = &s_aps[s_ap_sel];
            if (strcmp(ap->ssid, s_wf_ssid) != 0) {
                strlcpy(s_wf_ssid, ap->ssid, sizeof(s_wf_ssid));
                s_wf_pass[0] = '\0';
                s_wf_dirty = true;
            }
            s_wf_row = ap->secure ? WF_PASS : WF_SAVE;
            wf_message(ap->secure ? "Now type the password, then Save." : "Open network: just Save.", C_FG);
        }
        s_screen = SCREEN_WIFI;
        return true;
    case KEY_ESC:
    case KEY_BACKSPACE:
    case KEY_LEFT:
        s_screen = SCREEN_WIFI;
        return true;
    default:
        return false;
    }
}
#endif

static bool inspect_file(void);
static void flash_and_run(void);
static void draw_progress(size_t done, size_t total);

// Rescan the current folder, keeping the selection where possible.
static void rescan_dir(void)
{
    char name[64] = "";
    int sel = s_sel;
    entry_t *e = item_count() ? item_entry(s_sel) : NULL;
    if (e) {
        strlcpy(name, e->name, sizeof(name));
    }
    if (!is_dir(s_cwd)) { // the browser removed this folder
        strlcpy(s_cwd, SDCARD_MOUNT_POINT, sizeof(s_cwd));
        name[0] = '\0';
        sel = 0;
    }
    scan_dir();
    int n = item_count();
    s_sel = sel < n ? sel : (n > 0 ? n - 1 : 0);
    for (int i = 0; name[0] && i < n; i++) {
        entry_t *it = item_entry(i);
        if (it && strcmp(it->name, name) == 0) {
            s_sel = i;
            break;
        }
    }
}

// ---- copy / move / delete (Tab) ---------------------------------------------------------------
enum { FA_COPY, FA_CUT, FA_PASTE, FA_DELETE, FA_FORGET };
static int s_fa_items[5], s_fa_count, s_fa_sel;
static char s_fa_path[PATH_MAX_LEN + 64]; // the file or folder the menu / delete screen is about
static bool s_fa_is_dir;

static void note(lcd_color_t color, const char *fmt, const char *arg)
{
    snprintf(s_note, sizeof(s_note), fmt, arg);
    s_note_color = color;
}

// The selected file or folder (not ".."); fills s_fa_path.
static entry_t *selected_entry(void)
{
    entry_t *e = item_count() ? item_entry(s_sel) : NULL;
    if (e) {
        snprintf(s_fa_path, sizeof(s_fa_path), "%s/%s", s_cwd, e->name);
        s_fa_is_dir = e->is_dir;
    }
    return e;
}

// The loader itself on the RP2350: the boot stub loads this menu from it.
static bool protected_path(const char *path)
{
#if PICO_RP2040
    (void)path;
    return false;
#else
    return strcasecmp(path, "/pml2350.uf2") == 0;
#endif
}

static void select_name(const char *name)
{
    for (int i = 0; i < item_count(); i++) {
        entry_t *e = item_entry(i);
        if (e && strcmp(e->name, name) == 0) {
            s_sel = i;
            return;
        }
    }
}

static void mark(bool cut)
{
    entry_t *e = selected_entry();
    if (!e) {
        return;
    }
    if (cut && protected_path(s_fa_path)) {
        note(C_ERR, "%s is the loader menu; it stays here.", e->name);
        return;
    }
    strlcpy(s_clip, s_fa_path, sizeof(s_clip));
    s_clip_cut = cut;
}

static void ask_delete(void)
{
    entry_t *e = selected_entry();
    if (!e) {
        return;
    }
    if (protected_path(s_fa_path)) {
        note(C_ERR, "%s is the loader menu; delete it from a PC.", e->name);
        s_screen = SCREEN_BROWSE;
        return;
    }
    s_screen = SCREEN_DELETE;
}

static void draw_delete(void)
{
    draw_dialog(112);
    int y = LIST_Y + 4;
    lcd_text(MARGIN, y, s_fa_is_dir ? "Delete this folder?" : "Delete this file?", C_ERR, FONT_LARGE);
    y += 2 * ROW_H;
    draw_kv(y, s_fa_is_dir ? "Folder" : "File", strrchr(s_fa_path, '/') + 1);
    y += 2 * ROW_H;
    if (s_fa_is_dir) {
        draw_wrapped(y, "Everything in it is deleted too. This can't be undone.", C_FG);
    } else {
        draw_wrapped(y, "This can't be undone.", C_FG);
    }
    draw_footer(NULL, C_FG, "Enter Delete  Esc Cancel");
}

static void do_delete(void)
{
    char err[96], name[64];
    strlcpy(name, strrchr(s_fa_path, '/') + 1, sizeof(name));
    draw_footer(NULL, C_FG, "Deleting...");
    lcd_flush();
    if (fileops_delete(s_fa_path, err, sizeof(err))) {
        note(C_OK, "Deleted %s", name);
        if (strcasecmp(s_clip, s_fa_path) == 0) {
            s_clip[0] = '\0';
        }
    } else {
        note(C_ERR, "%s", err);
    }
    rescan_dir();
    s_screen = SCREEN_BROWSE;
}

#define COPY_NAME_Y (LIST_Y + 4 + 5 * ROW_H)
static char s_copy_shown[64];

static void copy_progress(const char *name, uint64_t done, uint64_t total)
{
#if PICOMULTILOADER_WIFI
    if (web_running()) {
        webserver_poll(); // keep the browser's connections alive during a long copy
    }
#endif
    if (strcmp(name, s_copy_shown) != 0) {
        strlcpy(s_copy_shown, name, sizeof(s_copy_shown));
        char line[COLS + 1];
        fit(line, name, COLS);
        lcd_fill_rect(0, COPY_NAME_Y, LCD_W, ROW_H, C_BG);
        lcd_text(MARGIN, COPY_NAME_Y, line, C_FG, FONT_LARGE);
        lcd_flush_rows(COPY_NAME_Y, ROW_H);
    }
    draw_progress((size_t)(done / 1024), (size_t)(total / 1024 ? total / 1024 : 1));
}

// "name (2).ext", "name (3).ext"... for a copy whose name is taken in the folder shown.
static bool free_name(char *out, size_t len, const char *name, bool is_dir)
{
    snprintf(out, len, "%s/%s", s_cwd, name);
    const char *dot = is_dir ? NULL : strrchr(name, '.');
    int base = dot && dot != name ? (int)(dot - name) : (int)strlen(name);
    for (int n = 2; fileops_exists(out); n++) {
        if (n > 99) {
            return false;
        }
        snprintf(out, len, "%s/%.*s (%d)%s", s_cwd, base, name, n, name + base);
    }
    return true;
}

static void paste(void)
{
    if (!s_clip[0]) {
        note(C_DIM, "%sNothing to paste: copy (C) or cut (X) a file first.", "");
        return;
    }
    const char *name = strrchr(s_clip, '/') + 1;
    if (!fileops_exists(s_clip)) {
        note(C_ERR, "%s is gone; nothing to paste.", name);
        s_clip[0] = '\0';
        return;
    }
    bool folder = is_dir(s_clip);
    size_t src_len = strlen(s_clip);
    if (folder && strncasecmp(s_cwd, s_clip, src_len) == 0 && (s_cwd[src_len] == '\0' || s_cwd[src_len] == '/')) {
        note(C_ERR, "Can't put %s inside itself.", name);
        return;
    }
    bool same_folder = (size_t)(name - 1 - s_clip) == strlen(s_cwd) && strncasecmp(s_clip, s_cwd, strlen(s_cwd)) == 0;
    // In the UF2 reader's buffer (idle while pasting): the main stack is small.
    size_t scratch_len;
    char *dst = uf2_scratch(&scratch_len), *shown = dst + PATH_MAX_LEN + 80;
    _Static_assert(2 * PATH_MAX_LEN + 176 <= 2048, "paste scratch");
    char err[96];
    if (s_clip_cut) {
        if (same_folder) {
            note(C_DIM, "%s is already in this folder.", name);
            return;
        }
        snprintf(dst, PATH_MAX_LEN + 80, "%s/%s", s_cwd, name);
        if (fileops_exists(dst)) {
            note(C_ERR, "There is already a %s here.", name);
            return;
        }
        if (!fileops_move(s_clip, dst, err, sizeof(err))) {
            note(C_ERR, "%s", err);
            return;
        }
        note(C_OK, "Moved %s here", name);
        s_clip[0] = '\0';
    } else {
        if (!free_name(dst, PATH_MAX_LEN + 80, name, folder)) {
            note(C_ERR, "Too many copies of %s here.", name);
            return;
        }
        draw_header();
        lcd_text(MARGIN, LIST_Y + 4, folder ? "Copying folder..." : "Copying...", C_ACCENT, FONT_LARGE);
        sd_name(shown, PATH_MAX_LEN + 96, s_clip);
        draw_kv(LIST_Y + 4 + 2 * ROW_H, "From", shown);
        sd_name(shown, PATH_MAX_LEN + 96, dst);
        draw_kv(LIST_Y + 4 + 3 * ROW_H, "To", shown);
        draw_footer(NULL, C_FG, "Please wait");
        lcd_flush();
        s_copy_shown[0] = '\0';
        draw_progress(0, 1);
        if (!fileops_copy(s_clip, dst, copy_progress, err, sizeof(err))) {
            note(C_ERR, "%s", err);
            rescan_dir();
            return;
        }
        note(C_OK, "Copied to %s", strrchr(dst, '/') + 1);
    }
    scan_dir();
    select_name(strrchr(dst, '/') + 1);
}

static void enter_files(void)
{
    entry_t *e = selected_entry();
    s_fa_count = 0;
    if (e) {
        s_fa_items[s_fa_count++] = FA_COPY;
        s_fa_items[s_fa_count++] = FA_CUT;
    }
    if (s_clip[0]) {
        s_fa_items[s_fa_count++] = FA_PASTE;
    }
    if (e) {
        s_fa_items[s_fa_count++] = FA_DELETE;
    }
    if (s_clip[0]) {
        s_fa_items[s_fa_count++] = FA_FORGET;
    }
    if (!s_fa_count) {
        note(C_DIM, "%sSelect a file or folder first.", "");
        return;
    }
    s_fa_sel = 0;
    s_screen = SCREEN_FILES;
}

static void draw_files(void)
{
    draw_header();
    int y = LIST_Y + 4;
    entry_t *e = item_count() ? item_entry(s_sel) : NULL;
    char buf[PATH_MAX_LEN + 80];
    if (e) {
        draw_kv(y, e->is_dir ? "Folder" : "File", e->name);
    } else {
        lcd_text(MARGIN, y, "This folder", C_DIM, FONT_LARGE);
    }
    y += 2 * ROW_H;
    for (int i = 0; i < s_fa_count; i++, y += WF_ROW_H) {
        char label[COLS + 1];
        switch (s_fa_items[i]) {
        case FA_COPY:
            strlcpy(buf, "Copy  (C)", sizeof(buf));
            break;
        case FA_CUT:
            strlcpy(buf, "Cut, to move it  (X)", sizeof(buf));
            break;
        case FA_PASTE:
            snprintf(buf, sizeof(buf), "Paste %s here  (V)", strrchr(s_clip, '/') + 1);
            break;
        case FA_DELETE:
            strlcpy(buf, "Delete  (Del)", sizeof(buf));
            break;
        default:
            snprintf(buf, sizeof(buf), "Forget the %s item", s_clip_cut ? "cut" : "copied");
            break;
        }
        fit(label, buf, COLS);
        draw_option(y, label, i == s_fa_sel);
    }
    if (s_clip[0]) {
        y += ROW_H;
        char where[PATH_MAX_LEN + 72];
        sd_name(where, sizeof(where), s_clip);
        snprintf(buf, sizeof(buf), "%s: %s", s_clip_cut ? "Cut" : "Copied", where);
        draw_wrapped(y, buf, C_DIM);
    }
    draw_footer(NULL, C_FG, "Up/Down Move  Enter Select  Esc Back");
}

static bool files_key(int key)
{
    switch (key) {
    case KEY_UP:
        s_fa_sel = s_fa_sel > 0 ? s_fa_sel - 1 : s_fa_count - 1;
        return true;
    case KEY_DOWN:
        s_fa_sel = s_fa_sel + 1 < s_fa_count ? s_fa_sel + 1 : 0;
        return true;
    case KEY_ESC:
    case KEY_BACKSPACE:
    case KEY_LEFT:
    case KEY_TAB:
        s_screen = SCREEN_BROWSE;
        return true;
    case KEY_ENTER:
    case KEY_RIGHT:
        break;
    default:
        return false;
    }
    s_screen = SCREEN_BROWSE;
    switch (s_fa_items[s_fa_sel]) {
    case FA_COPY:
        mark(false);
        break;
    case FA_CUT:
        mark(true);
        break;
    case FA_PASTE:
        paste();
        break;
    case FA_DELETE:
        ask_delete();
        break;
    default:
        s_clip[0] = '\0';
        break;
    }
    return true;
}

// ---- Home: the apps ---------------------------------------------------------------------------
enum { TAB_ALL, TAB_RECENT, TAB_FAVS, TAB_CAT }; // then one per category
#define TABS       (TAB_CAT + APP_CATS - 1)
#define TAB_Y      (BAR_H + 4)
#define GRID_Y     40
#define GRID_COLS  4
#define GRID_ROWS  4
#define CELL_W     (LCD_W / GRID_COLS)
#define CELL_H     58
#define FILTER_MAX 16
#define RUN_KEY    "run_path" // "size.date.time|path" of the .uf2 flashed last, to know it is installed

static uint8_t s_shown[APPS_MAX];     // the apps on the tab (indexes into s_apps.list), in order
static int s_tab, s_hsel, s_htop;     // s_htop: the first grid row shown
static int s_running = -1;            // the installed app, -1 = none of these
static char s_filter[FILTER_MAX + 1]; // typed: search all the apps
static int s_info_for = -1;           // the app s_app_info is about
static app_info_t s_app_info;

// The file's size and time with its path: it is the installed app while this matches.
static bool run_stamp(const char *path, char *out, size_t len)
{
    FILINFO fi;
    if (f_stat(sd_path(path), &fi) != FR_OK) {
        return false;
    }
    snprintf(out, len, "%lu.%u.%u|%s", (unsigned long)fi.fsize, fi.fdate, fi.ftime, path);
    return true;
}

// The .uf2 that was flashed last, if it is still the app in flash and unchanged on the card.
static bool installed_path(char *out, size_t len)
{
    char have[160], want[160], name[8], version[8];
    const char *bar;
    if (!load_setting(RUN_KEY, have, sizeof(have)) || !(bar = strchr(have, '|')) ||
        !uf2_installed(name, sizeof(name), version, sizeof(version)) || !run_stamp(bar + 1, want, sizeof(want)) ||
        strcasecmp(want, have) != 0) {
        return false;
    }
    strlcpy(out, bar + 1, len);
    return true;
}

static bool is_installed(const char *path)
{
    char run[PATH_MAX_LEN + 64];
    return installed_path(run, sizeof(run)) && strcasecmp(run, path) == 0;
}

static const char *tab_name(int tab)
{
    return tab == TAB_ALL ? "All" : tab == TAB_RECENT ? "Recent" : tab == TAB_FAVS ? "Favourites"
                                                                   : apps_cat_name(tab - TAB_CAT + 1);
}

static bool contains(const char *s, const char *part) // ignoring case
{
    size_t n = strlen(part);
    for (; *s; s++) {
        if (strncasecmp(s, part, n) == 0) {
            return true;
        }
    }
    return !n;
}

static bool on_tab(const app_t *e)
{
    if (s_filter[0]) {
        return contains(apps_str(&s_apps, e->title), s_filter);
    }
    switch (s_tab) {
    case TAB_ALL:
        return !(e->flags & APP_EXTRA);
    case TAB_RECENT:
        return e->recent >= 0;
    case TAB_FAVS:
        return e->flags & APP_FAV;
    default:
        return e->cat == s_tab - TAB_CAT + 1;
    }
}

static int home_index(void) // the selected app in s_apps.list, -1 = none
{
    return s_hsel < s_shown_n ? s_shown[s_hsel] : -1;
}

// Fill s_shown for the tab or the search; `keep` (a s_apps.list index) stays selected if shown.
static void home_build(int keep)
{
    s_shown_n = 0;
    for (int i = 0; i < s_apps.count; i++) {
        if (on_tab(&s_apps.list[i])) {
            s_shown[s_shown_n++] = (uint8_t)i;
        }
    }
    if (s_tab == TAB_RECENT && !s_filter[0]) { // the last run first
        for (int i = 1; i < s_shown_n; i++) {
            uint8_t v = s_shown[i];
            int j = i;
            for (; j > 0 && s_apps.list[s_shown[j - 1]].recent > s_apps.list[v].recent; j--) {
                s_shown[j] = s_shown[j - 1];
            }
            s_shown[j] = v;
        }
    }
    s_hsel = s_htop = 0;
    for (int i = 0; i < s_shown_n; i++) {
        if (s_shown[i] == keep) {
            s_hsel = i;
        }
    }
    s_info_for = -1;
}

// Show Home, reading the apps from the card again.
static void go_home(void)
{
    char keep[PATH_MAX_LEN] = "", run[PATH_MAX_LEN + 64], bin[PATH_MAX_LEN + 64];
    int sel = home_index();
    if (sel >= 0) {
        strlcpy(keep, apps_str(&s_apps, s_apps.list[sel].path), sizeof(keep));
    }
    free_entries();
    s_base = s_screen = SCREEN_HOME;
    s_apps.count = 0;
    if (s_sd_ok) {
        apps_scan(&s_apps);
    }
    bool have_run = installed_path(run, sizeof(run));
    s_running = sel = -1;
    for (int i = 0; i < s_apps.count; i++) {
        const app_t *e = &s_apps.list[i];
        if (have_run && s_running < 0 && apps_binary_path(&s_apps, e, bin, sizeof(bin)) && strcasecmp(bin, run) == 0) {
            s_running = i;
        }
        if (keep[0] && strcasecmp(apps_str(&s_apps, e->path), keep) == 0) {
            sel = i;
        }
    }
    home_build(sel);
}

// Show the file list in s_cwd, with the file flashed last selected if it is there.
static void go_files(void)
{
    if (!is_dir(s_cwd)) {
        strlcpy(s_cwd, SDCARD_MOUNT_POINT, sizeof(s_cwd));
    }
    s_base = s_screen = SCREEN_BROWSE;
    scan_dir();
    char last_file[64];
    if (load_setting("last_file", last_file, sizeof(last_file))) {
        select_name(last_file);
    }
    save_setting("last_dir", sd_path(s_cwd));
}

// Read the card again for the screen under the others (after USB drive mode, the web server...).
static void reload_base(void)
{
    if (s_base == SCREEN_HOME) {
        go_home();
    } else {
        rescan_dir();
    }
}

// No card: try it again (Enter).
static void retry_card(void)
{
    sdcard_unmount();
    s_sd_ok = sdcard_mount();
    config_reload();
    apps_reload();
    strlcpy(s_cwd, SDCARD_MOUNT_POINT, sizeof(s_cwd));
    reload_base();
}

static void draw_home(void)
{
    draw_bar();
    char buf[PATH_MAX_LEN + 80];
    if (s_filter[0]) {
        snprintf(buf, sizeof(buf), "Search: %s_", s_filter);
        lcd_text(MARGIN, TAB_Y, buf, C_BRIGHT, FONT_SMALL);
        snprintf(buf, sizeof(buf), "%d found  Esc clears", s_shown_n);
        lcd_text(LCD_W - MARGIN - 6 * (int)strlen(buf), TAB_Y, buf, C_DIM, FONT_SMALL);
    } else {
        for (int t = 0, x = 4; t < TABS; t++) {
            const char *name = tab_name(t);
            int w = 6 * (int)strlen(name) + 6;
            if (t == s_tab) {
                lcd_fill_rect(x, TAB_Y - 2, w, 13, C_SEL_BG);
            }
            lcd_text(x + 3, TAB_Y, name, t == s_tab ? C_SEL_FG : C_DIM, FONT_SMALL);
            x += w + 1;
        }
    }
    lcd_hline(MARGIN - 4, GRID_Y - 5, LCD_W - 2 * (MARGIN - 4), C_LINE);

    if (!s_sd_ok) {
        lcd_text(MARGIN, LIST_Y + 2 * ROW_H, "No SD card found.", C_ERR, FONT_LARGE);
        lcd_text(MARGIN, LIST_Y + 4 * ROW_H, "Insert a FAT32 card and press", C_DIM, FONT_LARGE);
        lcd_text(MARGIN, LIST_Y + 5 * ROW_H, "Enter to retry.", C_DIM, FONT_LARGE);
    } else if (!s_shown_n) {
        lcd_text(MARGIN, LIST_Y + 2 * ROW_H,
                 s_filter[0]            ? "No app matches."
                 : s_tab == TAB_RECENT  ? "No app run yet."
                 : s_tab == TAB_FAVS    ? "No favourites yet."
                 : s_tab == TAB_ALL     ? "No apps on the card."
                                        : "No apps in this category.",
                 C_DIM, FONT_LARGE);
        draw_wrapped(LIST_Y + 4 * ROW_H,
                     s_tab == TAB_FAVS  ? "F2 adds the selected app, or a file or folder in Files (F3)."
                     : s_tab == TAB_ALL ? "Put .uf2 files, or folders with one in, in the root of the card."
                                        : "",
                     C_DIM);
    }

    int row = s_hsel / GRID_COLS;
    if (row < s_htop) {
        s_htop = row;
    } else if (row >= s_htop + GRID_ROWS) {
        s_htop = row - GRID_ROWS + 1;
    }
    for (int k = 0; k < GRID_COLS * GRID_ROWS && s_htop * GRID_COLS + k < s_shown_n; k++) {
        int i = s_htop * GRID_COLS + k, idx = s_shown[i];
        const app_t *e = &s_apps.list[idx];
        int cx = k % GRID_COLS * CELL_W, cy = GRID_Y + k / GRID_COLS * CELL_H;
        bool sel = i == s_hsel;
        if (sel) {
            lcd_fill_rect(cx + 3, cy, CELL_W - 6, CELL_H - 2, C_FIELD);
            look_march(cx + 3, cy, CELL_W - 6, CELL_H - 2);
        }
        apps_draw_icon(&s_apps, e, cx + (CELL_W - 32) / 2, cy + 5);
        if (e->flags & APP_FAV) {
            lcd_text(cx + CELL_W - 15, cy + 3, "*", C_WARN, FONT_SMALL);
        }
        if (idx == s_running) {
            lcd_fill_rect(cx + 10, cy + 6, 4, 4, C_OK);
        }
        char label[13];
        fit(label, apps_str(&s_apps, e->title), 12);
        lcd_text(cx + (CELL_W - 6 * (int)strlen(label)) / 2, cy + 41, label, sel ? C_BRIGHT : C_FG, FONT_SMALL);
    }
    int rows = (s_shown_n + GRID_COLS - 1) / GRID_COLS;
    if (rows > GRID_ROWS) { // scroll bar
        int track = GRID_ROWS * CELL_H - 2, h = track * GRID_ROWS / rows;
        lcd_fill_rect(LCD_W - 3, GRID_Y + (track - h) * s_htop / (rows - GRID_ROWS), 2, h, C_DIM);
    }

    int idx = home_index();
    lcd_color_t color = C_DIM;
    buf[0] = '\0';
    if (s_note[0]) {
        strlcpy(buf, s_note, sizeof(buf));
        color = s_note_color;
    } else if (idx == s_running && idx >= 0) {
        snprintf(buf, sizeof(buf), "%s is installed: Enter starts it", apps_str(&s_apps, s_apps.list[idx].title));
        color = C_OK;
    } else if (idx >= 0) {
        const app_t *e = &s_apps.list[idx];
        if (s_info_for != idx) {
            apps_info(&s_apps, e, &s_app_info);
            s_info_for = idx;
        }
        if ((e->flags & APP_DIR) && e->binary == APP_NONE) {
            snprintf(buf, sizeof(buf), "Enter shows the files in SD%s", apps_str(&s_apps, e->path));
        } else if (s_app_info.desc[0] || s_app_info.version[0]) {
            snprintf(buf, sizeof(buf), "%s%s%s", s_app_info.version, s_app_info.version[0] ? "  " : "",
                     s_app_info.desc);
        } else {
            snprintf(buf, sizeof(buf), "SD%s", apps_str(&s_apps, e->path));
        }
    }
    char line[(LCD_W - 2 * MARGIN) / 6 + 1];
    fit(line, buf, (LCD_W - 2 * MARGIN) / 6);
    draw_footer(line, color,
#if PICOMULTILOADER_WIFI
                "Enter Run  F1 Control  F2 Fav  F3 Files  F4 Web  F5 Term"
#else
                "Enter Run  F1 Control  F2 Fav  F3 Files  F5 Terminal"
#endif
    );
}

// Start the installed app as it is (s_sel_path is its file).
static void run_installed(void)
{
#if PICOMULTILOADER_WIFI
    if (web_running()) {
        webserver_stop();
    }
#endif
    apps_add_recent(s_sel_path);
    draw_header();
    lcd_text(MARGIN, LIST_Y + 4, "Starting...", C_ACCENT, FONT_LARGE);
    draw_kv(LIST_Y + 4 + 2 * ROW_H, "File", s_sel_name);
    draw_footer(NULL, C_FG, "No flashing needed: it is installed");
    lcd_flush();
    uf2_boot_installed();
}

static void home_open(void)
{
    if (!s_sd_ok) {
        retry_card();
        return;
    }
    int idx = home_index();
    if (idx < 0) {
        return;
    }
    const app_t *e = &s_apps.list[idx];
    if (!apps_binary_path(&s_apps, e, s_sel_path, sizeof(s_sel_path))) { // no .uf2, or several: show them
        strlcpy(s_cwd, apps_str(&s_apps, e->path), sizeof(s_cwd));
        go_files();
        return;
    }
    strlcpy(s_sel_name, strrchr(s_sel_path, '/') + 1, sizeof(s_sel_name));
    if (idx == s_running) {
        run_installed();
    } else {
        inspect_file();
    }
}

static void fav_toast(int r)
{
    toast(r < 0 ? "Could not save the favourites" : r ? "Added to Favourites" : "Removed from Favourites");
}

static void home_fav(void)
{
    int idx = home_index();
    if (idx < 0) {
        return;
    }
    app_t *e = &s_apps.list[idx];
    char bin[PATH_MAX_LEN + 64];
    const char *path = apps_str(&s_apps, e->path);
    // a .uf2 in the folder made a favourite in Files counts as the app
    bool has_bin = (e->flags & APP_DIR) && apps_binary_path(&s_apps, e, bin, sizeof(bin));
    int r = apps_toggle_fav(has_bin && apps_is_fav(bin) ? bin : path);
    fav_toast(r);
    if (apps_is_fav(path) || (has_bin && apps_is_fav(bin))) {
        e->flags |= APP_FAV;
    } else {
        e->flags &= (uint8_t)~APP_FAV;
    }
    if (s_tab == TAB_FAVS && !s_filter[0]) {
        home_build(idx);
    }
}

static void browse_fav(void)
{
    entry_t *e = selected_entry(); // fills s_fa_path
    if (e && !e->is_dir && !has_uf2_ext(e->name)) {
        toast("Only apps can be favourites");
    } else if (e) {
        int r = apps_toggle_fav(s_fa_path);
        fav_toast(r);
        e->fav = apps_is_fav(s_fa_path);
    }
}

#if PICOMULTILOADER_WIFI
static void web_key(void) // F4
{
#if PML_RES
    if (!uf2_res_ready()) { // the Wi-Fi files first: install them, or ask
        if (web_running()) {
            stop_web(); // it failed for the lack of them
        }
        enter_web();
        return;
    }
#endif
    if (web_running()) {
        s_screen = SCREEN_WEB; // already on in the background
    } else {
        enter_web();
    }
}
#endif

// ---- control center (F1) ----------------------------------------------------------------------
// A panel over the dimmed Home or file list: one row per setting, the battery at the bottom.
enum { CC_WIFI, CC_USB, CC_LIGHT, CC_IDLE, CC_THEME, CC_SETUP, CC_ROWS };
#define CC_FIRST   (PICOMULTILOADER_WIFI ? CC_WIFI : CC_USB)
#define CC_X       26
#define CC_Y       34
#define CC_W       (LCD_W - 2 * CC_X)
#define CC_H       (FOOTER_Y - 4 - CC_Y)
#define CC_ROW     22
#define CC_VAL_X   (CC_X + 110)
#define CC_BAR_W   70
#define LIGHT_STEP 25
#define LIGHT_MAX  250
#define LIGHT_DEF  200

static int s_cc_row = CC_FIRST;
static int s_light; // backlight level, 0 = not read from the settings yet

static const uint8_t IDLE_MINUTES[] = {0, 1, 2, 5, 10, 30}; // 0 = never
#define IDLE_OPTS ((int)sizeof(IDLE_MINUTES))
#define IDLE_DEF  3 // 5 minutes
static int s_idle_opt = -1; // index into IDLE_MINUTES, -1 = not read from the settings yet

static int idle_opt(void)
{
    if (s_idle_opt < 0) {
        char buf[8];
        int minutes = load_setting("idle", buf, sizeof(buf)) ? atoi(buf) : IDLE_MINUTES[IDLE_DEF];
        s_idle_opt = IDLE_DEF;
        for (int i = 0; i < IDLE_OPTS; i++) {
            if (IDLE_MINUTES[i] == minutes) {
                s_idle_opt = i;
            }
        }
    }
    return s_idle_opt;
}

static void set_idle(int opt)
{
    s_idle_opt = (opt + IDLE_OPTS) % IDLE_OPTS;
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", IDLE_MINUTES[s_idle_opt]);
    save_setting("idle", buf);
}

uint8_t ui_backlight(void)
{
    if (!s_light) {
        char buf[8];
        s_light = load_setting("backlight", buf, sizeof(buf)) ? atoi(buf) : LIGHT_DEF;
        s_light = s_light < LIGHT_STEP ? LIGHT_STEP : s_light > LIGHT_MAX ? LIGHT_MAX : s_light;
    }
    return (uint8_t)s_light;
}

static void set_backlight(int level)
{
    s_light = level < LIGHT_STEP ? LIGHT_STEP : level > LIGHT_MAX ? LIGHT_MAX : level;
    if (kbd_present()) {
        kbd_set_lcd_backlight((uint8_t)s_light);
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", s_light);
    save_setting("backlight", buf);
}

static void next_theme(void)
{
    char next[24];
    theme_next(next, sizeof(next));
    theme_load(next);
    save_setting("theme", theme_name());
}

static void enter_ctl(void)
{
    ui_backlight();
    s_screen = SCREEN_CTL;
}

static void draw_meter(int x, int y, int pct, lcd_color_t color)
{
    look_frame(x, y + 2, CC_BAR_W, 9, C_LINE);
    lcd_fill_rect(x + 2, y + 4, (CC_BAR_W - 4) * pct / 100, 5, color);
}

static void draw_ctl(void)
{
    if (s_base == SCREEN_HOME) {
        draw_home();
    } else {
        draw_browse();
    }
    look_dither(BAR_H + 1, LCD_H - BAR_H - 1);
    lcd_fill_rect(CC_X, CC_Y, CC_W, CC_H, C_PANEL);
    look_frame(CC_X, CC_Y, CC_W, CC_H, C_LINE);
    look_stripes(CC_X + 1, CC_Y + 1, CC_W - 2, 3);
    lcd_text(CC_X + 12, CC_Y + 12, "Control center", C_ACCENT, FONT_LARGE);

    char hint[(CC_W - 24) / 6 + 1] = "", buf[64];
#if PICOMULTILOADER_WIFI
    web_status_t st;
    s_web_rev_shown = webserver_status(&st); // web_tick() redraws when it changes
    s_web_drawn_us = time_us_64();
#endif
    int y = CC_Y + 40;
    for (int r = CC_FIRST; r < CC_ROWS; r++, y += CC_ROW) {
        bool sel = r == s_cc_row;
        if (sel) {
            lcd_fill_rect(CC_X + 6, y - 4, CC_W - 12, CC_ROW - 2, C_SEL_BG);
        }
        lcd_color_t fg = sel ? C_SEL_FG : C_FG, dim = sel ? C_SEL_FG : C_DIM;
        static const char *const LABELS[CC_ROWS] = {"Wi-Fi", "USB drive", "Backlight", "Idle screen", "Theme", "Setup"};
        lcd_text(CC_X + 14, y, LABELS[r], fg, FONT_LARGE);
        const char *h = "";
        switch (r) {
#if PICOMULTILOADER_WIFI
        case CC_WIFI: {
            const char *state = "Off";
            lcd_color_t color = dim;
            h = "Enter: Wi-Fi and the web server on";
            switch (st.state) {
            case WEB_OFF:
                break;
            case WEB_STARTING:
            case WEB_CONNECTING:
                state = "Starting...";
                color = sel ? fg : C_ACCENT;
                h = st.ssid[0] ? st.ssid : "Starting Wi-Fi...";
                break;
            case WEB_RUNNING:
                state = st.ip[0] ? "On" : "Reconnecting";
                color = sel ? fg : st.ip[0] ? C_OK : C_WARN;
                snprintf(buf, sizeof(buf), "http://%s/  %s", st.ip, st.ssid);
                h = st.ip[0] ? buf : st.ssid;
                break;
            default:
                state = "Failed";
                color = sel ? fg : C_ERR;
                h = st.error;
                break;
            }
            lcd_text(CC_VAL_X, y, state, color, FONT_LARGE);
            break;
        }
#endif
        case CC_USB:
            lcd_text(CC_VAL_X, y, "Off  >", dim, FONT_LARGE);
            h = "Enter: the SD card as a drive on a PC";
            break;
        case CC_LIGHT:
            draw_meter(CC_VAL_X, y, s_light * 100 / LIGHT_MAX, sel ? C_SEL_FG : C_ACCENT);
            snprintf(buf, sizeof(buf), "%d%%", s_light * 100 / LIGHT_MAX);
            lcd_text(CC_VAL_X + CC_BAR_W + 6, y, buf, fg, FONT_LARGE);
            h = "Left/Right: dimmer or brighter";
            break;
        case CC_IDLE:
            if (IDLE_MINUTES[idle_opt()]) {
                snprintf(buf, sizeof(buf), "< %d min >", IDLE_MINUTES[idle_opt()]);
            } else {
                strlcpy(buf, "< Never >", sizeof(buf));
            }
            lcd_text(CC_VAL_X, y, buf, fg, FONT_LARGE);
            h = "A clock, and the backlight dimmed";
            break;
        case CC_THEME: {
            char name[(CC_W - 110 - 14 - 28) / 7 + 1];
            fit(name, theme_name(), sizeof(name) - 1);
            snprintf(buf, sizeof(buf), "< %s >", name);
            lcd_text(CC_VAL_X, y, buf, fg, FONT_LARGE);
            h = "Own .pal files go in " THEME_DIR;
            break;
        }
        case CC_SETUP:
            lcd_text(CC_VAL_X, y, ">", dim, FONT_LARGE);
#if PICOMULTILOADER_WIFI
            h = "Wi-Fi network, start folder, help";
#else
            h = "Start folder, help";
#endif
            break;
        }
        if (sel) {
            fit(hint, h, sizeof(hint) - 1);
        }
    }

    y += 6;
    lcd_hline(CC_X + 6, y, CC_W - 12, C_LINE);
    y += 10;
    lcd_text(CC_X + 14, y, "Battery", C_FG, FONT_LARGE);
    if (s_battery >= 0) {
        draw_meter(CC_VAL_X, y, s_battery, s_charging ? C_OK : s_battery < 15 ? C_ERR : C_FG);
        snprintf(buf, sizeof(buf), s_charging ? "%d%% +" : "%d%%", s_battery);
        lcd_text(CC_VAL_X + CC_BAR_W + 6, y, buf, C_FG, FONT_LARGE);
    } else {
        lcd_text(CC_VAL_X, y, "unknown", C_DIM, FONT_LARGE);
    }
    lcd_text(CC_X + 12, CC_Y + CC_H - 16, hint, C_DIM, FONT_SMALL);
    draw_footer(NULL, C_FG, "Up/Down Move  Left/Right Change  Esc Close");
}

#if PICOMULTILOADER_WIFI
static void ctl_wifi(void)
{
    if (web_running()) {
        draw_footer(NULL, C_FG, "Stopping...");
        lcd_flush();
        stop_web();
        s_screen = SCREEN_CTL;
    } else if (web_start()) {
        s_screen = SCREEN_CTL; // it starts in the background: the row shows how it goes
    }
}
#endif

static void enter_term(void);

static bool ctl_key(int key)
{
    int dir = key == KEY_LEFT ? -1 : key == KEY_RIGHT || key == KEY_ENTER ? 1 : 0;
    switch (key) {
    case KEY_UP:
        s_cc_row = s_cc_row > CC_FIRST ? s_cc_row - 1 : CC_ROWS - 1;
        return true;
    case KEY_DOWN:
    case KEY_TAB:
        s_cc_row = s_cc_row + 1 < CC_ROWS ? s_cc_row + 1 : CC_FIRST;
        return true;
    case KEY_ESC:
    case KEY_BACKSPACE:
    case KEY_F1:
        s_screen = s_base;
        return true;
    case KEY_F5:
        enter_term();
        return true;
    case KEY_LEFT:
    case KEY_RIGHT:
    case KEY_ENTER:
        break;
    default:
        return false;
    }
    switch (s_cc_row) {
#if PICOMULTILOADER_WIFI
    case CC_WIFI:
        if (key == KEY_ENTER || (dir > 0) != web_running()) {
            ctl_wifi();
        }
        break;
#endif
    case CC_USB:
        if (key == KEY_ENTER) {
            enter_usb();
        }
        break;
    case CC_LIGHT:
        if (key == KEY_ENTER) { // steps up, then round to the dimmest
            set_backlight(s_light >= LIGHT_MAX ? LIGHT_STEP : s_light + LIGHT_STEP);
        } else {
            set_backlight(s_light + dir * LIGHT_STEP);
        }
        break;
    case CC_IDLE:
        set_idle(idle_opt() + dir);
        break;
    case CC_THEME:
        next_theme();
        break;
    case CC_SETUP:
        if (key != KEY_LEFT) {
            enter_wifi();
        }
        break;
    }
    return true;
}

// ---- terminal (F5) ----------------------------------------------------------------------------
// Output scrolls up from the bottom; the line being typed is in the footer. Everything printed also
// goes to the serial console, and the console's typing reaches here as keys.
#define TERM_X     4
#define TERM_COLS  ((LCD_W - 2 * TERM_X) / 6)
#define TERM_Y     (BAR_H + 4)
#define TERM_LH    10
#define TERM_ROWS  ((LCD_H - 2 - TERM_Y) / TERM_LH)
#define TERM_LINES 80 // kept for scrolling back
#define TERM_IN    96
#define TERM_HIST  8

static char s_term[TERM_LINES][TERM_COLS + 1]; // a ring: line i is s_term[i % TERM_LINES]
static uint8_t s_term_err[TERM_LINES];
static int s_term_n, s_term_back; // lines written; lines scrolled back
static char s_term_in[TERM_IN];
static char s_hist[TERM_HIST][TERM_IN];
static int s_hist_n, s_hist_at; // commands kept; the one recalled (s_hist_n = the new line)
static bool s_term_started;

static void term_line(const char *text, size_t n, uint8_t kind)
{
    int i = s_term_n++ % TERM_LINES;
    memcpy(s_term[i], text, n);
    s_term[i][n] = '\0';
    s_term_err[i] = kind;
}

// kind: 0 output, 1 error, 2 the command typed
static void term_put(const char *text, uint8_t kind)
{
    printf("%s\n", text);
    s_term_back = 0;
    size_t len = strlen(text);
    do { // wrap at the last space that fits (a typed command: at the column, as it was typed)
        size_t n = len > TERM_COLS ? TERM_COLS : len;
        if (n < len && kind != 2) {
            size_t sp = n;
            while (sp > 0 && text[sp] != ' ') {
                sp--;
            }
            n = sp > 0 ? sp : n;
        }
        term_line(text, n, kind);
        text += n;
        len -= n;
        while (*text == ' ' && kind != 2) {
            text++;
            len--;
        }
    } while (len);
}

static void term_out(const char *text, bool error)
{
    term_put(text, error);
}

static void term_prompt(char *out, size_t len)
{
    snprintf(out, len, "SD%s> ", sd_path(shell_cwd()));
}

static void enter_term(void)
{
    if (!s_term_started) {
        s_term_started = true;
        term_put("PicoMultiLoader terminal. help lists the commands; F5 or exit closes it.", 0);
    }
    s_hist_at = s_hist_n;
    s_screen = SCREEN_TERM;
}

static void draw_term(void)
{
    draw_bar();
    // The prompt and what is being typed follow the output, like any terminal, wrapped at the
    // column, with a block cursor after them (on a line of its own when the last one is full).
    char line[TERM_IN + PATH_MAX_LEN + 8];
    term_prompt(line, sizeof(line));
    strlcat(line, s_term_in, sizeof(line));
    int in_len = (int)strlen(line), in_rows = in_len / TERM_COLS + 1;
    int rows = TERM_ROWS, kept = s_term_n < TERM_LINES ? s_term_n : TERM_LINES;
    int total = kept + in_rows;
    int max_back = total > rows ? total - rows : 0;
    s_term_back = s_term_back > max_back ? max_back : s_term_back;
    int first = total - rows - s_term_back; // the top line shown, counting from the oldest kept
    for (int r = 0; r < rows; r++) {
        int k = first + r, y = TERM_Y + r * TERM_LH;
        if (k < 0 || k >= total) {
            continue;
        }
        if (k < kept) {
            int i = (s_term_n - kept + k) % TERM_LINES;
            lcd_text(TERM_X, y, s_term[i], s_term_err[i] == 1 ? C_ERR : C_FG, FONT_SMALL);
            continue;
        }
        int at = (k - kept) * TERM_COLS;
        char part[TERM_COLS + 1];
        strlcpy(part, line + (at < in_len ? at : in_len), sizeof(part));
        int x = lcd_text(TERM_X, y, part, C_FG, FONT_SMALL);
        if (k == total - 1) {
            lcd_fill_rect(x, y - 1, 6, TERM_LH - 1, C_FG); // the cursor
        }
    }
    if (s_term_back) {
        lcd_fill_rect(LCD_W - 3, TERM_Y + (rows * TERM_LH) * (max_back - s_term_back) / (max_back + 1), 2, 8, C_DIM);
    }
}

static void term_history(void)
{
    for (int i = 0; i < s_hist_n; i++) {
        char buf[TERM_IN + 16];
        snprintf(buf, sizeof(buf), "%3d  %.*s", i + 1, TERM_IN - 1, s_hist[i]);
        term_put(buf, 0);
    }
}

static void term_remember(const char *cmd)
{
    if (!cmd[0] || (s_hist_n && strcmp(s_hist[s_hist_n - 1], cmd) == 0)) {
        return;
    }
    if (s_hist_n == TERM_HIST) {
        memmove(s_hist[0], s_hist[1], sizeof(s_hist[0]) * (TERM_HIST - 1));
        s_hist_n--;
    }
    strlcpy(s_hist[s_hist_n++], cmd, sizeof(s_hist[0]));
}

// Home's apps (Home shows them from now on, if the file list was under the terminal).
static void term_apps(void)
{
    if (s_base != SCREEN_HOME) {
        go_home();
        s_screen = SCREEN_TERM;
    }
}

static void term_list_apps(void)
{
    term_apps();
    for (int i = 0; i < s_apps.count; i++) {
        const app_t *e = &s_apps.list[i];
        char buf[TERM_COLS + 1];
        snprintf(buf, sizeof(buf), "%-20.20s %s%s", apps_str(&s_apps, e->title), apps_str(&s_apps, e->path),
                 i == s_running ? "  (installed)" : "");
        term_put(buf, 0);
    }
    if (!s_apps.count) {
        term_put(s_sd_ok ? "No apps on the card." : "No SD card.", !s_sd_ok);
    }
}

// run <app>: an app's name from Home, or a .uf2 file. Goes to the confirm screen.
static void term_run(const char *what)
{
    term_apps();
    const app_t *found = NULL;
    for (int i = 0; i < s_apps.count && !found; i++) {
        if (strcasecmp(apps_str(&s_apps, s_apps.list[i].title), what) == 0) {
            found = &s_apps.list[i];
        }
    }
    if (found && apps_binary_path(&s_apps, found, s_sel_path, sizeof(s_sel_path))) {
        // the app's .uf2
    } else if (!shell_resolve(s_sel_path, sizeof(s_sel_path), what) || !has_uf2_ext(s_sel_path) ||
               !fileops_exists(s_sel_path)) {
        char buf[TERM_IN + 40];
        snprintf(buf, sizeof(buf), found ? "run: %s has no single .uf2 to run" : "run: no app or .uf2 called %s", what);
        term_put(buf, 1);
        return;
    }
    strlcpy(s_sel_name, strrchr(s_sel_path, '/') + 1, sizeof(s_sel_name));
    inspect_file(); // the confirm screen (Enter runs), or the error screen
}

#if PICOMULTILOADER_WIFI
static void term_wifi(int argc, char **argv)
{
    char *ip[] = {"ip"};
    if (argc < 2) {
        shell_run(1, ip, term_out);
    } else if (strcasecmp(argv[1], "on") == 0) {
        if (web_running()) {
            term_put("Wi-Fi is on already.", 0);
        } else if (web_start()) {
            s_screen = SCREEN_TERM;
            term_put("Starting Wi-Fi and the web server: ip shows the address.", 0);
        }
    } else if (strcasecmp(argv[1], "off") == 0) {
        if (web_running()) {
            stop_web();
            s_screen = SCREEN_TERM;
        }
        term_put("Wi-Fi is off.", 0);
    } else {
        term_put("wifi: on or off?", 1);
    }
}
#endif

static bool open_viewer(screen_t from);
static void screenshot(void);

// view <file>: the text reader or the image viewer; Esc comes back here
static void term_view(int argc, char **argv)
{
    if (argc < 2) {
        term_put("view: which file?", 1);
    } else if (!shell_resolve(s_sel_path, sizeof(s_sel_path), argv[1]) || !fileops_exists(s_sel_path)) {
        term_put("view: no such file", 1);
    } else {
        strlcpy(s_sel_name, strrchr(s_sel_path, '/') + 1, sizeof(s_sel_name));
        if (!open_viewer(SCREEN_TERM)) {
            term_put("view: text files (.txt, .md, ...) and .bmp images only", 1);
        }
    }
}

static void term_exec(void)
{
    char cmd[TERM_IN], line[TERM_IN + PATH_MAX_LEN + 8];
    strlcpy(cmd, s_term_in, sizeof(cmd));
    s_term_in[0] = '\0';
    term_prompt(line, sizeof(line));
    strlcat(line, cmd, sizeof(line));
    term_put(line, 2);
    term_remember(cmd);
    s_hist_at = s_hist_n;

    char *argv[SHELL_ARGS];
    int argc = shell_split(cmd, argv, SHELL_ARGS);
    if (!argc) {
        return;
    }
    const char *c = argv[0];
    if (strcasecmp(c, "clear") == 0) {
        s_term_n = s_term_back = 0;
    } else if (strcasecmp(c, "history") == 0) {
        term_history();
    } else if (strcasecmp(c, "exit") == 0) {
        s_screen = s_base;
    } else if (strcasecmp(c, "home") == 0) {
        go_home();
    } else if (strcasecmp(c, "files") == 0) {
        strlcpy(s_cwd, shell_cwd(), sizeof(s_cwd));
        go_files();
    } else if (strcasecmp(c, "ctl") == 0) {
        enter_ctl();
    } else if (strcasecmp(c, "usb") == 0) {
        enter_usb();
    } else if (strcasecmp(c, "apps") == 0) {
        term_list_apps();
    } else if (strcasecmp(c, "view") == 0) {
        term_view(argc, argv);
    } else if (strcasecmp(c, "shot") == 0) {
        screenshot();
    } else if (strcasecmp(c, "run") == 0) {
        if (argc < 2) {
            term_put("run: which app?", 1);
        } else {
            term_run(argv[1]);
        }
#if PICOMULTILOADER_WIFI
    } else if (strcasecmp(c, "wifi") == 0) {
        term_wifi(argc, argv);
#endif
    } else if (!shell_run(argc, argv, term_out)) {
        snprintf(line, sizeof(line), "%s: no such command. help lists them.", c);
        term_put(line, 1);
    }
}

static bool term_key(int key)
{
    size_t len = strlen(s_term_in);
    switch (key) {
    case KEY_ENTER:
        term_exec();
        return true;
    case KEY_BACKSPACE:
    case KEY_DEL:
        if (len) {
            s_term_in[len - 1] = '\0';
        }
        return true;
    case KEY_TAB:
        shell_complete(s_term_in, sizeof(s_term_in), term_out);
        return true;
    case KEY_UP:
        if (s_hist_at > 0) {
            strlcpy(s_term_in, s_hist[--s_hist_at], sizeof(s_term_in));
        }
        return true;
    case KEY_DOWN:
        if (s_hist_at < s_hist_n) {
            s_hist_at++;
            strlcpy(s_term_in, s_hist_at < s_hist_n ? s_hist[s_hist_at] : "", sizeof(s_term_in));
        }
        return true;
    case KEY_PAGE_UP:
        s_term_back += TERM_ROWS - 1; // clamped when drawing
        return true;
    case KEY_PAGE_DOWN:
        s_term_back = s_term_back > TERM_ROWS - 1 ? s_term_back - (TERM_ROWS - 1) : 0;
        return true;
    case KEY_ESC:
        if (len) {
            s_term_in[0] = '\0';
            s_hist_at = s_hist_n;
        } else {
            s_screen = s_base;
        }
        return true;
    case KEY_F5:
        s_screen = s_base;
        return true;
    case KEY_F1:
        enter_ctl();
        return true;
    default:
        if (key >= 0x20 && key < 0x7F && len + 1 < sizeof(s_term_in)) {
            s_term_in[len] = (char)key;
            s_term_in[len + 1] = '\0';
            return true;
        }
        return false;
    }
}

// ---- text reader and image viewer (from Files, or view in the terminal) -----------------------
#define TEXT_X    4
#define TEXT_Y    (PATH_Y + 18)
#define TEXT_LH   10
#define TEXT_COLS ((LCD_W - 2 * TEXT_X) / 6)
#define TEXT_ROWS ((FOOTER_Y - 2 - TEXT_Y) / TEXT_LH)

static screen_t s_view_from; // where Esc goes back to
static bool s_image_drawn;   // the image is on screen (redraws would decode it again)
static bool s_image_grey;

// Open s_sel_path in its viewer. False if there is none for it.
static bool open_viewer(screen_t from)
{
    view_kind_t kind = view_kind(s_sel_path);
    if (kind == VIEW_NONE) {
        return false;
    }
    s_view_from = from;
    if (kind == VIEW_IMAGE) {
        s_image_drawn = false;
        s_screen = SCREEN_IMAGE;
        return true;
    }
    char err[64];
    if (!text_open(s_sel_path, kind == VIEW_MARKDOWN, TEXT_COLS, TEXT_ROWS, err, sizeof(err))) {
        snprintf(s_error, sizeof(s_error), "%.60s: %s", s_sel_name, err);
        s_screen = SCREEN_ERROR;
        return true;
    }
    s_screen = SCREEN_TEXT;
    return true;
}

static void close_viewer(void)
{
    text_close();
    if (s_screen == SCREEN_IMAGE) {
        theme_apply();
    }
    s_screen = s_view_from;
}

static void draw_text(void)
{
    draw_bar();
    char title[COLS + 1];
    fit(title, s_sel_name, COLS);
    lcd_text(MARGIN, PATH_Y, title, C_ACCENT, FONT_LARGE);
    lcd_hline(MARGIN - 4, PATH_Y + 16, LCD_W - 2 * (MARGIN - 4), C_LINE);
    text_draw(TEXT_X, TEXT_Y, TEXT_LH);
    char line[40];
    if (text_at_end()) {
        snprintf(line, sizeof(line), "Page %d (end)", text_page() + 1);
    } else {
        snprintf(line, sizeof(line), "Page %d  %d%%", text_page() + 1, text_percent());
    }
    draw_footer(line, C_DIM, "Up/Down Page  Home/End  Esc Close");
}

static bool text_key(int key)
{
    switch (key) {
    case KEY_DOWN:
    case KEY_RIGHT:
    case KEY_PAGE_DOWN:
    case KEY_ENTER:
    case ' ':
        return text_go(text_page() + 1);
    case KEY_UP:
    case KEY_LEFT:
    case KEY_PAGE_UP:
        return text_go(text_page() - 1);
    case KEY_HOME:
        return text_go(0);
    case KEY_END:
        draw_footer("Finding the end...", C_DIM, "");
        lcd_flush();
        text_end();
        return true;
    case KEY_ESC:
    case KEY_BACKSPACE:
        close_viewer();
        return true;
    default:
        return false;
    }
}

static void draw_image(void)
{
    if (s_image_drawn) {
        return;
    }
    look_anim_reset(); // the palette is the image's now
    char err[64];
    s_image_drawn = true;
    if (!image_show(s_sel_path, s_image_grey, err, sizeof(err))) {
        theme_apply();
        snprintf(s_error, sizeof(s_error), "%.60s: %s", s_sel_name, err);
        s_screen = SCREEN_ERROR;
        ui_redraw();
    }
}

// Left/Right in an image opened from Files: the image before or after it in the folder.
static void image_step(int dir)
{
    if (s_view_from != SCREEN_BROWSE || !s_count) {
        return;
    }
    int at = -1;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_entries[i].name, s_sel_name) == 0) {
            at = i;
        }
    }
    for (int k = 1; k < s_count; k++) {
        int i = ((at + dir * k) % s_count + s_count) % s_count;
        const entry_t *e = &s_entries[i];
        if (!e->is_dir && view_kind(e->name) == VIEW_IMAGE) {
            snprintf(s_sel_path, sizeof(s_sel_path), "%s/%s", s_cwd, e->name);
            strlcpy(s_sel_name, e->name, sizeof(s_sel_name));
            s_sel = i + (at_root() ? 0 : 1);
            s_image_drawn = false;
            return;
        }
    }
}

static bool image_key(int key)
{
    switch (key) {
    case KEY_LEFT:
    case KEY_UP:
        image_step(-1);
        return !s_image_drawn;
    case KEY_RIGHT:
    case KEY_DOWN:
    case ' ':
        image_step(1);
        return !s_image_drawn;
    case 'g':
    case 'G':
        s_image_grey = !s_image_grey;
        s_image_drawn = false;
        return true;
    case KEY_ESC:
    case KEY_BACKSPACE:
    case KEY_ENTER:
        close_viewer();
        return true;
    default:
        return false;
    }
}

// ---- idle screen ------------------------------------------------------------------------------
// After the idle time without a key: a dim clock on black with the backlight turned down, drawn
// once a minute and nothing in between (the main loop sleeps meanwhile). It moves a little each
// minute, and after IDLE_OFF_US more the backlight goes off. A key wakes it and does nothing else.
#define IDLE_OFF_US   (10ull * 60 * 1000000)
#define DIGIT_W       34
#define DIGIT_H       62
#define SEG           7
#define IDLE_BLOCK_H  (DIGIT_H + 16 + 3 * 18) // the clock and the lines under it

static bool s_idle, s_idle_off;
static uint64_t s_last_key_us, s_idle_since_us;
static int s_idle_minute;

// One 7-segment digit (or the colon, d = 10) with its top-left at x, y. Returns the next x.
#define SEG_M ((DIGIT_H - SEG) / 2)
static int idle_digit(int x, int y, int d, lcd_color_t color)
{
    // segments a..g: top, top right, bottom right, bottom, bottom left, top left, middle
    static const uint8_t SEGS[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
    static const uint8_t RECT[7][4] = {
        {0, 0, DIGIT_W, SEG},           {DIGIT_W - SEG, 0, SEG, SEG_M + SEG}, {DIGIT_W - SEG, SEG_M, SEG, DIGIT_H - SEG_M},
        {0, DIGIT_H - SEG, DIGIT_W, SEG}, {0, SEG_M, SEG, DIGIT_H - SEG_M},     {0, 0, SEG, SEG_M + SEG},
        {0, SEG_M, DIGIT_W, SEG},
    };
    if (d == 10) {
        lcd_fill_rect(x + 2, y + DIGIT_H / 3 - SEG / 2, SEG, SEG, color);
        lcd_fill_rect(x + 2, y + 2 * DIGIT_H / 3 - SEG / 2, SEG, SEG, color);
        return x + SEG + 10;
    }
    for (int k = 0; k < 7; k++) {
        if (SEGS[d] & (1 << k)) {
            lcd_fill_rect(x + RECT[k][0], y + RECT[k][1], RECT[k][2], RECT[k][3], color);
        }
    }
    return x + DIGIT_W + 8;
}

// The minute the idle clock shows: of the day once the time is known, else since power-on
static int idle_minute(void)
{
    if (clock_known()) {
        int64_t t = clock_utc() + clock_offset();
        return (int)(t / 60 % 1440);
    }
    return (int)(time_us_64() / 60000000u);
}

static void idle_draw(void)
{
    clock_tm_t tm;
    bool known = clock_local(&tm);
    int minutes = idle_minute();
    s_idle_minute = minutes;
    char clock[8], date[24], battery[24] = "";
    if (known) {
        snprintf(clock, sizeof(clock), "%02d:%02d", tm.hour, tm.min);
        clock_date_text(&tm, date, sizeof(date));
    } else { // no time from the web yet: show how long we have been on
        snprintf(clock, sizeof(clock), "%d:%02d", minutes / 60 % 100, minutes % 60);
        strlcpy(date, "on for (h:mm)", sizeof(date));
    }
    if (s_battery >= 0) {
        snprintf(battery, sizeof(battery), s_charging ? "battery %d%% charging" : "battery %d%%", s_battery);
    }
    int w = 0;
    for (const char *c = clock; *c; c++) {
        w += *c == ':' ? SEG + 10 : DIGIT_W + 8;
    }
    // a different place each minute, spread over the screen (h * 37 % range: hops about)
    int h = minutes * 37 + 11;
    int xr = LCD_W - (w - 8) - 2 * MARGIN, yr = LCD_H - IDLE_BLOCK_H - 2 * MARGIN;
    int x = MARGIN + (xr > 0 ? h % xr : 0), y = MARGIN + (h / 7) % yr;
    lcd_fill_rect(0, 0, LCD_W, LCD_H, 0);
    int cx = x;
    for (const char *c = clock; *c; c++) {
        cx = idle_digit(cx, y, *c == ':' ? 10 : *c - '0', 2);
    }
    const char *lines[] = {date, battery, PICOMULTILOADER_WIFI && !known ? "Wi-Fi on sets the clock" : ""};
    int mid = x + (w - 8) / 2;
    for (int i = 0; i < 3; i++) {
        int lx = mid - 7 * (int)strlen(lines[i]) / 2;
        lcd_text(lx < 0 ? 0 : lx, y + DIGIT_H + 16 + i * 18, lines[i], 1, FONT_LARGE);
    }
}

static void idle_backlight(int level)
{
    if (kbd_present()) {
        kbd_set_lcd_backlight((uint8_t)level);
    }
}

static void idle_start(void)
{
    s_idle = true;
    s_idle_off = false;
    s_idle_since_us = time_us_64();
    look_anim_reset();
    lcd_set_palette(0, RGB565(0, 0, 0));
    lcd_set_palette(1, RGB565(110, 110, 120)); // the lines
    lcd_set_palette(2, RGB565(170, 170, 180)); // the digits
    int dim = ui_backlight() / 8;
    idle_backlight(dim < 6 ? 6 : dim);
    idle_draw();
    lcd_flush();
}

static void idle_wake(void)
{
    if (!s_idle) {
        return;
    }
    s_idle = false;
    idle_backlight(ui_backlight());
    if (s_screen == SCREEN_IMAGE) {
        s_image_drawn = false; // decode it again, with its palette
    } else {
        theme_apply();
    }
    ui_redraw();
}

static void idle_tick(void)
{
    uint64_t now = time_us_64();
    if (!s_idle) {
        int minutes = IDLE_MINUTES[idle_opt()];
        if (minutes && s_screen != SCREEN_USB && now - s_last_key_us > (uint64_t)minutes * 60000000u) {
            idle_start();
        }
        return;
    }
    if (s_idle_off) {
        return; // nothing to see: draw nothing
    }
    if (now - s_idle_since_us > IDLE_OFF_US) {
        s_idle_off = true;
        idle_backlight(0);
    } else if (idle_minute() != s_idle_minute) {
        idle_draw();
        lcd_flush();
    }
}

// ---- screenshots (F10, Shift+F5) and exit messages ---------------------------------------------
static void screenshot(void)
{
    char path[40], msg[sizeof(s_toast)];
    if (!s_sd_ok || s_screen == SCREEN_USB) {
        toast("Screenshots need the SD card");
    } else if (shot_save(path, sizeof(path))) {
        printf("ui: saved %s\n", path);
        snprintf(msg, sizeof(msg), "Saved %s", path);
        toast(msg);
    } else {
        toast("Could not save the screenshot");
    }
}

#define EXIT_FILE PML_DIR "/exit.txt"

// What the app that ran last left for us: /pml/exit.txt (its first line), else its exit code in
// the watchdog scratch registers. Shown for a few seconds over the first screen.
static void exit_message(void)
{
    char text[sizeof(s_toast)] = "";
    int32_t code;
    bool coded = uf2_exit_code(&code);
    FIL *f = s_sd_ok ? malloc(sizeof(FIL)) : NULL;
    if (f && f_open(f, EXIT_FILE, FA_READ) == FR_OK) {
        char line[96];
        if (f_gets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = '\0';
            fit(text, line, sizeof(text) - 1);
        }
        f_close(f);
        f_unlink(EXIT_FILE);
    }
    free(f);
    if (!text[0] && coded) {
        snprintf(text, sizeof(text), "The app ended with code %ld", (long)code);
    }
    if (text[0]) {
        printf("ui: the app says: %s\n", text);
        toast(text);
        s_toast_until = time_us_64() + 5000000;
    }
}

// Returns false if the key isn't used here.
static bool home_key(int key)
{
    int n = s_shown_n;
    bool had_note = s_note[0];
    s_note[0] = '\0';
    switch (key) {
    case KEY_LEFT:
        s_hsel -= s_hsel > 0;
        return true;
    case KEY_RIGHT:
        s_hsel += s_hsel + 1 < n;
        return true;
    case KEY_UP:
        s_hsel -= s_hsel >= GRID_COLS ? GRID_COLS : 0;
        return true;
    case KEY_DOWN: // to the last app if the row below is short
        s_hsel = s_hsel + GRID_COLS < n ? s_hsel + GRID_COLS : s_hsel / GRID_COLS < (n - 1) / GRID_COLS ? n - 1 : s_hsel;
        return true;
    case KEY_PAGE_UP:
        s_hsel = s_hsel >= GRID_COLS * GRID_ROWS ? s_hsel - GRID_COLS * GRID_ROWS : s_hsel % GRID_COLS;
        return true;
    case KEY_PAGE_DOWN:
        s_hsel = s_hsel + GRID_COLS * GRID_ROWS < n ? s_hsel + GRID_COLS * GRID_ROWS : (n ? n - 1 : 0);
        return true;
    case KEY_HOME:
        s_hsel = 0;
        return true;
    case KEY_END:
        s_hsel = n ? n - 1 : 0;
        return true;
    case KEY_TAB:
        if (!s_filter[0]) {
            s_tab = (s_tab + 1) % TABS;
        }
        s_filter[0] = '\0';
        home_build(home_index());
        return true;
    case KEY_BACKSPACE:
        if (s_filter[0]) {
            s_filter[strlen(s_filter) - 1] = '\0';
            home_build(home_index());
        }
        return true;
    case KEY_ESC:
        if (s_filter[0]) {
            s_filter[0] = '\0';
            home_build(home_index());
            return true;
        }
        return had_note;
    case KEY_ENTER:
        home_open();
        return true;
    case KEY_F1:
        enter_ctl();
        return true;
    case KEY_F2:
        home_fav();
        return true;
    case KEY_F3:
        go_files();
        return true;
#if PICOMULTILOADER_WIFI
    case KEY_F4:
        web_key();
        return true;
#endif
    case KEY_F5:
        enter_term();
        return true;
    default:
        if ((key > ' ' && key < 0x7F) || (key == ' ' && s_filter[0])) {
            if (strlen(s_filter) < FILTER_MAX) {
                size_t len = strlen(s_filter);
                s_filter[len] = (char)key;
                s_filter[len + 1] = '\0';
                home_build(home_index());
            }
            return true;
        }
        return had_note;
    }
}

#if PICOMULTILOADER_WIFI
// Runs on every screen: the server keeps going in the background.
static void web_tick(void)
{
    if (!web_running()) {
        if (s_icon_shown != C_BG && s_screen != SCREEN_USB) {
            ui_redraw(); // drop the icon
        }
        return;
    }
    char path[sizeof(s_sel_path)];
    if (webserver_take_flash_request(path, sizeof(path))) {
        // let the reply reach the browser
        for (uint64_t until = time_us_64() + 300000; time_us_64() < until;) {
            webserver_poll();
        }
        stop_web();
        idle_wake();
        strlcpy(s_sel_path, path, sizeof(s_sel_path));
        strlcpy(s_sel_name, strrchr(path, '/') + 1, sizeof(s_sel_name));
        if (inspect_file()) {
            flash_and_run(); // reboots on success, else shows the error screen
        } else {
            ui_redraw();
        }
        return;
    }
    if (s_screen == SCREEN_WEB || s_screen == SCREEN_CTL) {
        web_status_t st;
        // redraw on changes, at most 4 times a second (uploads update the progress a lot)
        if (webserver_status(&st) != s_web_rev_shown && time_us_64() - s_web_drawn_us > 250000) {
            ui_redraw();
        }
        return;
    }
    uint32_t changes = webserver_changes();
    if (s_screen == s_base && changes != s_web_changes_seen) {
        s_web_changes_seen = changes;
        config_reload();
        apps_reload();
        reload_base();
        ui_redraw();
    } else if (web_icon_color() != s_icon_shown) {
        web_status_t st;
        webserver_status(&st);
        if (web_icon_color() == C_OK) {
            char msg[sizeof(s_toast)];
            snprintf(msg, sizeof(msg), "Web server at http://%s/", st.ip);
            toast(msg);
        }
        ui_redraw();
    }
}
#endif

void ui_tick(void)
{
    if (s_toast[0] && time_us_64() >= s_toast_until) {
        s_toast[0] = '\0';
        ui_redraw();
    }
    idle_tick();
    if (!s_idle) {
        look_tick();
        if (s_screen != SCREEN_IMAGE && bar_minute() != s_bar_minute) { // the image viewer has no bar
            draw_bar_clock();
            lcd_flush();
        }
    }
#if PICOMULTILOADER_WIFI
    web_tick();
#endif
    if (s_screen != SCREEN_USB) {
        return;
    }
    usbdrive_state_t st = usbdrive_state();
    if (st == USBDRIVE_EJECTED) {
        leave_usb();
        ui_redraw();
    } else if (st != s_usb_shown) {
        ui_redraw();
    }
}

void ui_redraw(void)
{
    if (s_idle) {
        return; // idle_tick() draws
    }
    switch (s_screen) {
    case SCREEN_HOME:
        draw_home();
        break;
    case SCREEN_BROWSE:
        draw_browse();
        break;
    case SCREEN_CONFIRM:
        draw_confirm();
        break;
    case SCREEN_ERROR:
        draw_error();
        break;
    case SCREEN_USB:
        draw_usb();
        break;
    case SCREEN_WIFI:
        draw_wifi();
        break;
    case SCREEN_HELP:
        draw_help();
        break;
    case SCREEN_PICK:
        draw_pick();
        break;
    case SCREEN_FILES:
        draw_files();
        break;
    case SCREEN_DELETE:
        draw_delete();
        break;
    case SCREEN_CTL:
        draw_ctl();
        break;
    case SCREEN_TERM:
        draw_term();
        break;
    case SCREEN_TEXT:
        draw_text();
        break;
    case SCREEN_IMAGE:
        draw_image(); // it flushes, and nothing goes over it
        return;
#if PML_RES
    case SCREEN_RES:
        draw_res();
        break;
#endif
#if PICOMULTILOADER_WIFI
    case SCREEN_WEB:
        draw_web();
        break;
    case SCREEN_WIFI_SCAN:
        draw_wifi_scan();
        break;
#else
    default:
        break;
#endif
    }
    if (s_toast[0]) {
        draw_toast();
    }
    lcd_flush();
}

// ---- flashing ---------------------------------------------------------------------------------
#define PROG_X MARGIN
#define PROG_Y 170
#define PROG_W (LCD_W - 2 * MARGIN)
#define PROG_H 16

static void draw_progress(size_t done, size_t total)
{
    static int last = -1;
    int pct = total ? (int)((uint64_t)done * 100 / total) : 0;
    if (pct == last || (pct % 2 && done != total)) {
        return;
    }
    last = done == total ? -1 : pct;

    look_frame(PROG_X, PROG_Y, PROG_W, PROG_H, C_LINE);
    look_stripes(PROG_X + 2, PROG_Y + 2, (PROG_W - 4) * pct / 100, PROG_H - 4);
    look_step(); // the stripes move as it goes
    char txt[16];
    snprintf(txt, sizeof(txt), "%d%%", pct);
    lcd_fill_rect(0, PROG_Y + PROG_H + 6, LCD_W, 14, C_BG);
    lcd_text((LCD_W - 7 * (int)strlen(txt)) / 2, PROG_Y + PROG_H + 6, txt, C_FG, FONT_LARGE);
    lcd_flush_rows(PROG_Y, PROG_H + 6 + 14);
}

static void flash_and_run(void)
{
#if PICOMULTILOADER_WIFI
    if (web_running()) {
        webserver_stop(); // nothing should touch the card or the radio while flashing
    }
#endif
    char err[96];
    draw_header();
    lcd_text(MARGIN, LIST_Y + 4, "Flashing...", C_ACCENT, FONT_LARGE);
    draw_kv(LIST_Y + 4 + 2 * ROW_H, "File", s_sel_name);
    char buf[72];
    snprintf(buf, sizeof(buf), "%s %s", s_info.name[0] ? s_info.name : "(unnamed)", s_info.version);
    draw_kv(LIST_Y + 4 + 3 * ROW_H, "App", buf);
    draw_footer(NULL, C_FG, "Do not switch off");
    lcd_flush();
    draw_progress(0, 1);

    if (!uf2_flash(s_sel_path, &s_info, draw_progress, err, sizeof(err))) {
        snprintf(s_error, sizeof(s_error), "Flashing failed: %s. The previous app may be damaged; "
                 "flash another one.", err);
        s_screen = SCREEN_ERROR;
        ui_redraw();
        return;
    }
    save_setting("last_file", s_sel_name);
    char stamp[160];
    if (run_stamp(s_sel_path, stamp, sizeof(stamp))) {
        save_setting(RUN_KEY, stamp);
    }
    apps_add_recent(s_sel_path);

    lcd_text(MARGIN, PROG_Y + PROG_H + 30, "Done. Starting app...", C_OK, FONT_LARGE);
    lcd_flush();
    sleep_ms(400);
    uf2_boot_installed();
}

// ---- input ------------------------------------------------------------------------------------
// Check s_sel_path; go to the confirm screen if it can be flashed, else to the error screen.
static bool inspect_file(void)
{
    char err[96];
    if (!uf2_inspect(s_sel_path, &s_info, err, sizeof(err))) {
        snprintf(s_error, sizeof(s_error), "%s", err);
        s_screen = SCREEN_ERROR;
        return false;
    }
    s_confirm_run = is_installed(s_sel_path);
    s_screen = SCREEN_CONFIRM;
    return true;
}

static void open_selected(void)
{
    if (!s_sd_ok) {
        retry_card();
        return;
    }
    if (item_count() == 0) {
        return;
    }
    entry_t *e = item_entry(s_sel);
    if (!e) {
        change_dir(NULL);
        return;
    }
    if (e->is_dir) {
        change_dir(e->name);
        return;
    }

    snprintf(s_sel_path, sizeof(s_sel_path), "%s/%s", s_cwd, e->name);
    strlcpy(s_sel_name, e->name, sizeof(s_sel_name));
    if (!open_viewer(SCREEN_BROWSE)) {
        inspect_file();
    }
}

void ui_key(int key)
{
    s_last_key_us = time_us_64();
    if (s_idle) {
        idle_wake(); // the key only wakes it
        return;
    }
    if (key == KEY_F10 || key == KEY_F10_ALT) {
        screenshot();
        ui_redraw();
        return;
    }
    switch (s_screen) {
    case SCREEN_HOME:
        if (!home_key(key)) {
            return;
        }
        break;
    case SCREEN_BROWSE: {
        int n = item_count();
        bool had_note = s_note[0];
        s_note[0] = '\0';
        switch (key) {
        case KEY_UP:
            s_sel = s_sel > 0 ? s_sel - 1 : (n > 0 ? n - 1 : 0);
            break;
        case KEY_DOWN:
            s_sel = s_sel + 1 < n ? s_sel + 1 : 0;
            break;
        case KEY_PAGE_UP:
            s_sel = s_sel > LIST_ROWS ? s_sel - LIST_ROWS : 0;
            break;
        case KEY_PAGE_DOWN:
            s_sel = s_sel + LIST_ROWS < n ? s_sel + LIST_ROWS : (n > 0 ? n - 1 : 0);
            break;
        case KEY_HOME:
            s_sel = 0;
            break;
        case KEY_END:
            s_sel = n > 0 ? n - 1 : 0;
            break;
        case KEY_ENTER:
        case KEY_RIGHT:
            open_selected();
            break;
        case KEY_BACKSPACE:
        case KEY_LEFT:
            if (at_root()) {
                go_home();
            } else {
                change_dir(NULL);
            }
            break;
        case KEY_F1:
            enter_ctl();
            break;
        case KEY_F2:
            browse_fav();
            break;
        case KEY_F3:
            go_home();
            break;
#if PICOMULTILOADER_WIFI
        case KEY_F4:
            web_key();
            break;
#endif
        case KEY_F5:
            enter_term();
            break;
        case KEY_TAB:
            enter_files();
            break;
        case KEY_DEL:
            ask_delete();
            break;
        case 'c':
        case 'C':
            mark(false);
            break;
        case 'x':
        case 'X':
            mark(true);
            break;
        case 'v':
        case 'V':
            paste();
            break;
        case KEY_ESC: // forget the copied item, else back to Home
            if (s_clip[0]) {
                s_clip[0] = '\0';
            } else {
                go_home();
            }
            break;
        default:
            if (!had_note) {
                return;
            }
            break;
        }
        break;
    }
    case SCREEN_CONFIRM:
        if (key == KEY_ENTER && s_confirm_run) {
            run_installed();
            return;
        }
        if (key == KEY_ENTER || (s_confirm_run && (key == 'r' || key == 'R'))) {
            flash_and_run();
            return;
        }
        if (key == KEY_ESC || key == KEY_BACKSPACE || key == KEY_LEFT) {
            s_screen = s_base;
            break;
        }
        return;
    case SCREEN_USB:
        if (key == KEY_ESC) {
            leave_usb();
            break;
        }
        return;
    case SCREEN_WIFI:
        if (!wifi_key(key)) {
            return;
        }
        break;
    case SCREEN_HELP:
        if (!help_key(key)) {
            return;
        }
        break;
    case SCREEN_PICK:
        if (!pick_key(key)) {
            return;
        }
        break;
    case SCREEN_FILES:
        if (!files_key(key)) {
            return;
        }
        break;
    case SCREEN_CTL:
        if (!ctl_key(key)) {
            return;
        }
        break;
    case SCREEN_TERM:
        if (!term_key(key)) {
            return;
        }
        break;
    case SCREEN_TEXT:
        if (!text_key(key)) {
            return;
        }
        break;
    case SCREEN_IMAGE:
        if (!image_key(key)) {
            return;
        }
        break;
    case SCREEN_DELETE:
        if (key == KEY_ENTER) {
            do_delete();
            break;
        }
        if (key == KEY_ESC || key == KEY_BACKSPACE || key == KEY_LEFT) {
            s_screen = SCREEN_BROWSE;
            break;
        }
        return;
#if PML_RES
    case SCREEN_RES:
        if (!res_key(key)) {
            return;
        }
        break;
#else
    case SCREEN_RES:
        s_screen = s_base;
        break;
#endif
#if PICOMULTILOADER_WIFI
    case SCREEN_WIFI_SCAN:
        if (!wifi_scan_key(key)) {
            return;
        }
        break;
    case SCREEN_WEB: {
        web_status_t st;
        webserver_status(&st);
        if (key == KEY_F4 || (key == KEY_ESC && st.state == WEB_FAILED)) {
            draw_footer(NULL, C_FG, "Stopping...");
            lcd_flush();
            stop_web();
            break;
        }
        if (key == KEY_ESC || key == KEY_BACKSPACE || key == KEY_LEFT) {
            s_screen = s_base; // the server keeps running
            break;
        }
        return;
    }
#else
    case SCREEN_WEB:
    case SCREEN_WIFI_SCAN:
        s_screen = s_base;
        break;
#endif
    case SCREEN_ERROR:
        if (key == KEY_ESC || key == KEY_ENTER || key == KEY_BACKSPACE || key == KEY_LEFT) {
            s_screen = s_base;
            break;
        }
        return;
    }
    ui_redraw();
}

void ui_set_battery(int percent, bool charging)
{
    if (percent != s_battery || charging != s_charging) {
        s_battery = percent;
        s_charging = charging;
        ui_redraw();
    }
}

// ---- boot screen ------------------------------------------------------------------------------
#define BOOT_LINES (4 + PICOMULTILOADER_WIFI + PML_RES)
#define BOOT_Y     144
#define BOOT_BAR_Y (BOOT_Y + 15 * BOOT_LINES + 5)

// "PML", 5x7 cells per letter, each row's bits from the left
static const uint8_t LOGO[3][7] = {
    {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11},
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},
};

static void load_theme(void)
{
    char name[24];
    theme_load(load_setting("theme", name, sizeof(name)) ? name : NULL);
}

void ui_boot(bool sd_ok)
{
    s_sd_ok = sd_ok;
    load_theme();
    s_boot_line = 0;
    look_anim_reset();
    lcd_fill_rect(0, 0, LCD_W, LCD_H, C_BG);
    // the logo's cells are striped in the cycled colours, so it shimmers
    for (int l = 0; l < 3; l++) {
        for (int r = 0; r < 7; r++) {
            for (int c = 0; c < 5; c++) {
                if (LOGO[l][r] & (0x10 >> c)) {
                    look_stripes(100 + 42 * l + 7 * c, 44 + 7 * r, 6, 6);
                }
            }
        }
    }
    lcd_text((LCD_W - 15 * 7) / 2, 104, "PicoMultiLoader", C_BRIGHT, FONT_LARGE);
    char sub[32];
    snprintf(sub, sizeof(sub), "PicoCalc  %s", uf2_chip_name());
    lcd_text((LCD_W - 6 * (int)strlen(sub)) / 2, 122, sub, C_DIM, FONT_SMALL);
    ui_boot_check("SD card", sd_ok ? "OK" : "missing", sd_ok);
}

void ui_boot_check(const char *what, const char *result, bool ok)
{
    if (s_boot_line < 0 || s_boot_line >= BOOT_LINES) {
        return;
    }
    int y = BOOT_Y + 15 * s_boot_line++;
    for (int x = lcd_text(48, y, what, C_FG, FONT_SMALL) + 4; x < 196; x += 4) { // dotted leader
        lcd_fill_rect(x, y + 7, 1, 1, C_LINE);
    }
    char r[20];
    fit(r, result, sizeof(r) - 1);
    lcd_text(200, y, r, ok ? C_OK : C_WARN, FONT_SMALL);
    look_frame(48, BOOT_BAR_Y, 224, 8, C_LINE);
    look_stripes(50, BOOT_BAR_Y + 2, 220 * s_boot_line / BOOT_LINES, 4);
    lcd_flush();
}

#if PML_RES
static int s_boot_res_y;

static void boot_res_progress(size_t done, size_t total)
{
    static int last = -1;
    int pct = total ? (int)(done * 100 / total) : 0;
    if (pct == last) {
        return;
    }
    last = pct;
    char txt[8];
    snprintf(txt, sizeof(txt), "%d%%", pct);
    lcd_fill_rect(200, s_boot_res_y, LCD_W - 200, 10, C_BG);
    lcd_text(200, s_boot_res_y, txt, C_FG, FONT_SMALL);
    lcd_flush_rows(s_boot_res_y, 10);
}

// The Wi-Fi files: installed at switch-on when nothing is lost (the first boot of a new loader)
static void boot_res(bool sd_ok)
{
    res_state_t st = uf2_res_check();
    if ((st == RES_BLANK || st == RES_OLD) && sd_ok) {
        bool shown = s_boot_line >= 0 && s_boot_line < BOOT_LINES;
        if (shown) {
            s_boot_res_y = BOOT_Y + 15 * s_boot_line;
            lcd_text(48, s_boot_res_y, "Wi-Fi files", C_FG, FONT_SMALL);
            lcd_flush();
        }
        char err[96];
        bool ok = uf2_res_install(shown ? boot_res_progress : NULL, err, sizeof(err));
        if (!ok) {
            printf("ui: Wi-Fi files: %s\n", err);
        }
        if (shown) {
            lcd_fill_rect(200, s_boot_res_y, LCD_W - 200, 10, C_BG);
        }
        ui_boot_check("Wi-Fi files", ok ? "installed" : "failed", ok);
        return;
    }
    ui_boot_check("Wi-Fi files", st == RES_OK ? "OK" : st == RES_USED ? "F4 installs" : st == RES_NO_FILE ? "missing" : "no SD card",
                  st == RES_OK);
}
#endif

void ui_init(bool sd_ok)
{
    s_sd_ok = sd_ok;
    if (s_boot_line < 0) { // no boot screen
        load_theme();
    }
    strlcpy(s_cwd, SDCARD_MOUNT_POINT, sizeof(s_cwd));

    // Files opens the start folder from Setup, else the last directory, else the root
    char last[PATH_MAX_LEN];
    if (!load_setting("start_dir", s_start_dir, sizeof(s_start_dir))) {
        s_start_dir[0] = '\0';
    }
    if (s_start_dir[0] && is_dir(s_start_dir)) {
        copy_dir(s_cwd, sizeof(s_cwd), s_start_dir);
    } else if (load_setting("last_dir", last, sizeof(last)) && last[0] == '/' && is_dir(last)) {
        copy_dir(s_cwd, sizeof(s_cwd), last);
    }
    clock_load_setting();
    go_home();
    s_last_key_us = time_us_64();
    exit_message();
    for (int i = 0; i < s_shown_n; i++) { // select the app run last
        if (s_apps.list[s_shown[i]].recent == 0) {
            s_hsel = i;
        }
    }

#if PML_RES
    boot_res(sd_ok);
#endif
#if PICOMULTILOADER_WIFI
    // Wi-Fi autostart: bring the web server up in the background (the top icon shows its state)
    char web_auto[4];
    s_web_auto = load_setting("web_auto", web_auto, sizeof(web_auto)) && web_auto[0] == '1';
    bool web_ok = true, autostart = s_web_auto && sd_ok;
#if PML_RES
    bool files = uf2_res_ready(); // else F4 installs them first
#else
    bool files = true;
#endif
    if (autostart && files) {
        s_web_changes_seen = webserver_changes();
        if (!(web_ok = webserver_start())) {
            printf("ui: Wi-Fi autostart failed\n");
        }
    }
    ui_boot_check("Wi-Fi", !autostart ? "off" : !files ? "press F4" : web_ok ? "starting" : "failed",
                  web_ok && files);
#endif
    char found[16];
    snprintf(found, sizeof(found), "%d found", s_apps.count);
    ui_boot_check("Apps", sd_ok ? found : "-", sd_ok);
    if (s_boot_line >= 0) { // let the logo shimmer for a moment
        for (uint64_t until = time_us_64() + 500000; time_us_64() < until;) {
            look_tick();
#if PICOMULTILOADER_WIFI
            webserver_poll();
#endif
        }
        s_boot_line = -1;
    }
    ui_redraw();
}
