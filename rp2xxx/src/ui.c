// ui.c - on-screen file browser: pick a .uf2 on the SD card, confirm, flash it (or run it from RAM)
// and boot it through the UF2 Loader's stage3.
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "pico/stdlib.h"
#include "config.h"
#include "keyboard.h"
#include "lcd.h"
#include "sdcard.h"
#include "uf2_image.h"
#include "usb_drive.h"
#if PICOMULTILOADER_WIFI
#include "webserver.h"
#endif

// ---- colours / layout -------------------------------------------------------------------------
// The framebuffer holds palette indices; ui_init() loads the colours.
enum {
    C_BG,
    C_FG,
    C_DIM,
    C_ACCENT,
    C_SEL_BG,
    C_SEL_FG,
    C_ERR,
    C_OK,
    C_WARN,
    C_FIELD,   // text field background
    C_SIG_OFF, // unlit signal bars
};

#define MARGIN    8
#define TITLE_Y   6
#define PATH_Y    24
#define LIST_Y    46
#define ROW_H     15
#define FOOTER_Y  (LCD_H - 34)
#define LIST_ROWS ((FOOTER_Y - 4 - LIST_Y) / ROW_H)
#define COLS      ((LCD_W - 2 * MARGIN) / 7)

#define MAX_ENTRIES 256
#define PATH_MAX_LEN 256

// ---- state ------------------------------------------------------------------------------------
typedef struct {
    char *name;
    bool is_dir;
    uint32_t size;
} entry_t;

typedef enum {
    SCREEN_BROWSE,
    SCREEN_CONFIRM,
    SCREEN_ERROR,
    SCREEN_USB,
    SCREEN_WEB,
    SCREEN_WIFI,      // F1: Setup (the Wi-Fi network the web server joins, start folder, help)
    SCREEN_WIFI_SCAN, // pick a network from a scan
    SCREEN_HELP,      // how to use the loader (from F1)
    SCREEN_PICK,      // choose the start folder (from F1)
} screen_t;

static screen_t s_screen;
static bool s_sd_ok;
static char s_cwd[PATH_MAX_LEN]; // absolute FatFS path, "" for the root, e.g. "/apps"
static entry_t s_entries[MAX_ENTRIES];
static int s_count;
static int s_sel, s_top;
static int s_battery = -1;
static bool s_charging;

static char s_sel_path[PATH_MAX_LEN + 256];
static char s_sel_name[64];
static uf2_info_t s_info;
static char s_error[128];
static const char *s_path_override; // shown in the header instead of s_cwd

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
        if (hidden(&fi) || (!dir_entry && !has_uf2_ext(fi.fname))) {
            continue;
        }
        entry_t *e = &s_entries[s_count];
        e->name = strdup(fi.fname);
        e->is_dir = dir_entry;
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

// ---- web server icon (top bar, shown while the server is on) ----------------------------------
#if PICOMULTILOADER_WIFI
static lcd_color_t s_icon_shown; // colour of the icon last drawn, 0 (C_BG) = none

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

// A Wi-Fi fan, WEB_ICON_W x 11 pixels, top-left at (x, y).
#define WEB_ICON_W 15
static void draw_web_icon(int x, int y, lcd_color_t color)
{
    int cx = x + WEB_ICON_W / 2, cy = y + 10;
    for (int dy = 0; dy <= 10; dy++) {
        for (int dx = -WEB_ICON_W / 2; dx <= WEB_ICON_W / 2; dx++) {
            int r2 = dx * dx + dy * dy;
            bool dot = r2 <= 2;
            bool arc = 5 * abs(dx) <= 6 * dy && ((r2 >= 13 && r2 <= 28) || (r2 >= 49 && r2 <= 76));
            if (dot || arc) {
                lcd_fill_rect(cx + dx, cy - dy, 1, 1, color);
            }
        }
    }
}
#else
static bool web_running(void)
{
    return false;
}
#endif

// ---- drawing ----------------------------------------------------------------------------------
static void draw_header(void)
{
    lcd_fill_rect(0, 0, LCD_W, LCD_H, C_BG);
    lcd_text(MARGIN, TITLE_Y, "PicoMultiLoader", C_FG, FONT_LARGE);

    int right = LCD_W - MARGIN; // where the next item to the left of the battery ends
    if (s_battery >= 0) {
        // battery icon: 18x9 body + 2x5 nub, fill proportional to charge
        int bx = LCD_W - MARGIN - 20, by = TITLE_Y + 2;
        char pct[16];
        snprintf(pct, sizeof(pct), "%d%%", s_battery);
        lcd_text(bx - 4 - 7 * (int)strlen(pct), TITLE_Y, pct, C_FG, FONT_LARGE);
        lcd_fill_rect(bx, by, 18, 9, C_FG);
        lcd_fill_rect(bx + 1, by + 1, 16, 7, C_BG);
        lcd_fill_rect(bx + 18, by + 2, 2, 5, C_FG);
        lcd_fill_rect(bx + 2, by + 2, 14 * s_battery / 100, 5, s_charging ? C_OK : C_FG);
        right = bx - 4 - 7 * (int)strlen(pct) - 8;
    }
#if PICOMULTILOADER_WIFI
    s_icon_shown = web_icon_color();
    if (s_icon_shown != C_BG) {
        draw_web_icon(right - WEB_ICON_W, TITLE_Y + 1, s_icon_shown);
    }
#else
    (void)right;
#endif

    char path[COLS + 1], shown[PATH_MAX_LEN + 8];
    snprintf(shown, sizeof(shown), "Path: SD%s", s_path_override ? s_path_override : s_cwd);
    if (strlen(shown) == strlen("Path: SD")) {
        strcat(shown, "/");
    }
    fit(path, shown, COLS);
    lcd_text(MARGIN, PATH_Y, path, C_FG, FONT_LARGE);
    lcd_hline(MARGIN - 4, PATH_Y + 16, LCD_W - 2 * (MARGIN - 4), C_FG);
}

static void draw_footer(const char *line1, lcd_color_t line1_color, const char *keys)
{
    lcd_hline(MARGIN - 4, FOOTER_Y, LCD_W - 2 * (MARGIN - 4), C_FG);
    if (line1) {
        lcd_text(MARGIN, FOOTER_Y + 5, line1, line1_color, FONT_SMALL);
    }
    lcd_text(MARGIN, FOOTER_Y + 19, keys, C_FG, FONT_SMALL);
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
        lcd_text(MARGIN, LIST_Y + 2 * ROW_H, "No .uf2 files here.", C_DIM, FONT_LARGE);
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
        if (!e) {
            lcd_text(MARGIN, y, "[..]", fg, FONT_LARGE);
        } else if (e->is_dir) {
            snprintf(buf, sizeof(buf), "[%s]", e->name);
            fit(label, buf, COLS);
            lcd_text(MARGIN, y, label, fg, FONT_LARGE);
        } else {
            char size[12];
            format_size(size, sizeof(size), e->size);
            int size_cols = (int)strlen(size);
            fit(label, e->name, COLS - size_cols - 1);
            lcd_text(MARGIN, y, label, fg, FONT_LARGE);
            lcd_text(LCD_W - MARGIN - 7 * size_cols, y, size, sel ? C_SEL_FG : C_DIM, FONT_LARGE);
        }
    }

    // scroll bar
    if (n > LIST_ROWS) {
        int track = LIST_ROWS * ROW_H;
        int h = track * LIST_ROWS / n;
        int y = LIST_Y + (track - h) * s_top / (n - LIST_ROWS);
        lcd_fill_rect(LCD_W - 3, y - 1, 2, h, C_DIM);
    }

    char name[40], version[24], installed[80];
    if (!uf2_bootloader_ok()) {
        snprintf(installed, sizeof(installed), "UF2 Loader (stage3) not found - see Help");
    } else if (uf2_installed(name, sizeof(name), version, sizeof(version))) {
        snprintf(installed, sizeof(installed), "Installed: %s %s", name, version);
    } else {
        snprintf(installed, sizeof(installed), "Installed: (none)");
    }
    char line[(LCD_W - 2 * MARGIN) / 6 + 1];
    fit(line, installed, (LCD_W - 2 * MARGIN) / 6);
    draw_footer(line, uf2_bootloader_ok() ? C_DIM : C_WARN,
#if PICOMULTILOADER_WIFI
                "Enter open  F1 Setup  F4 Web  F5 USB drive"
#else
                "Enter open  F1 Setup  F5 USB drive"
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
    draw_header();
    char buf[96];
    int y = LIST_Y + 4;
    lcd_text(MARGIN, y, s_info.ram_app ? "Run this app?" : "Flash and run this app?", C_ACCENT, FONT_LARGE);
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
    if (s_info.ram_app) {
        draw_kv(y, "Type", "RAM app (runs without flashing)");
    } else {
        snprintf(buf, sizeof(buf), "flash, %u of %u KB", (unsigned)((s_info.addr_hi - s_info.addr_lo + 1023) / 1024),
                 (unsigned)(uf2_flash_capacity() / 1024));
        draw_kv(y, "Type", buf);
    }
    draw_footer(NULL, C_FG, s_info.ram_app ? "Enter: run    Esc: back" : "Enter: flash & run    Esc: back");
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
    draw_header();
    int y = LIST_Y + 4;
    lcd_text(MARGIN, y, "Can't run this file", C_ERR, FONT_LARGE);
    y += 2 * ROW_H;
    draw_kv(y, "File", s_sel_name);
    y += 2 * ROW_H;
    draw_wrapped(y, s_error, C_FG);
    draw_footer(NULL, C_FG, "Esc / Enter: back");
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
    draw_footer(NULL, C_FG, st == USBDRIVE_CONNECTED ? "Esc: stop (eject on the PC first!)"
                                                     : "Esc: back to the file list");
}

static void leave_usb(void)
{
    usbdrive_stop(); // remounts the card
    s_sd_ok = sdcard_mount();
    if (!s_sd_ok || !is_dir(s_cwd)) {
        strlcpy(s_cwd, SDCARD_MOUNT_POINT, sizeof(s_cwd));
    }
    scan_dir();
    s_screen = SCREEN_BROWSE;
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
    if (!usbdrive_start()) {
        s_sd_ok = sdcard_mount();
        scan_dir();
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
                lcd_fill_rect(MARGIN, y, w, 10, C_FG);
                lcd_fill_rect(MARGIN + 1, y + 1, w - 2, 8, C_BG);
                lcd_fill_rect(MARGIN + 2, y + 2, (w - 4) * st.progress / 100, 6, C_ACCENT);
            }
        }
        if (st.ap_mode) {
            draw_wrapped(FOOTER_Y - 3 * ROW_H, "Join this network, or save your own Wi-Fi in Setup (F1) to use that instead.", C_DIM);
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
                C_DIM, st.state == WEB_FAILED ? "F4 / Esc: stop" : "Esc: back to the files   F4: stop server");
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
    scan_dir();
    s_screen = SCREEN_BROWSE;
}

static void enter_web(void)
{
    if (!s_sd_ok) {
        snprintf(s_error, sizeof(s_error), "Insert an SD card first.");
        strlcpy(s_sel_name, "(SD card)", sizeof(s_sel_name));
        s_screen = SCREEN_ERROR;
        return;
    }
    s_web_changes_seen = webserver_changes();
    if (!webserver_start()) {
        snprintf(s_error, sizeof(s_error), "Could not start the web server: the Wi-Fi chip did not start.");
        strlcpy(s_sel_name, "(web server)", sizeof(s_sel_name));
        s_screen = SCREEN_ERROR;
        return;
    }
    s_screen = SCREEN_WEB;
}
#endif

// ---- Setup (F1) -------------------------------------------------------------------------------
#if PICOMULTILOADER_WIFI
enum { WF_SSID, WF_PASS, WF_SHOW, WF_SCAN, WF_SAVE, WF_FORGET, WF_AUTO, WF_START, WF_HELP, WF_ROWS };
#else
enum { WF_START, WF_HELP, WF_ROWS };
#endif
#define WF_ROW_H   17
#define WF_FIELD_X (MARGIN + 74)
#define WF_COLS    ((LCD_W - MARGIN - WF_FIELD_X - 4) / 7)
#define MAX_APS    24

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
    "#The file list",
    "Up/Down, PgUp/PgDn, Home/End: move.",
    "Enter or Right: open a folder, or show the details of a .uf2 file.",
    "Enter on the details screen: flash and run it. Esc: back.",
    "Backspace or Left: parent folder.",
    "The installed app starts after flashing, or on power-on.",
#if PICOMULTILOADER_WIFI
    "F1: Setup (Wi-Fi network, start folder) and this help.",
    "F4: web file manager. F5: USB drive mode.",
#else
    "F1: Setup (start folder) and this help.",
    "F5: USB drive mode.",
#endif
    "Only folders and .uf2 files are listed. The last folder and file are remembered.",
    "No card? Insert a FAT32 card and press Enter.",
    "",
    "#Installing the loader",
    "PicoMultiLoader runs on top of the PicoCalc UF2 Loader (pelrun/uf2loader): flash its bootloader once, then copy "
#if PICO_RP2040
    "BOOT2040.UF2"
#else
    "BOOT2350.UF2"
#endif
    " from this project to the root of the SD card.",
    "",
    "#Back to the loader",
    "Hold Up, F1 or F5 while switching on: the UF2 Loader opens this menu instead of the app.",
    "",
    "#Which files work",
#if PICO_RP2040
    "UF2 files for the RP2040, built to run from flash (the normal Pico SDK build) or from RAM (no_flash).",
#else
    "UF2 files for the RP2350 (ARM, secure), built to run from flash (the normal Pico SDK build) or from RAM (no_flash). RISC-V and RP2040 files are refused.",
#endif
    "Apps in flash keep working after a restart; RAM apps run once, without touching the installed app.",
    "Each file is checked before flashing; an unsuitable one shows why.",
    "",
    "#Setup (F1)",
#if PICOMULTILOADER_WIFI
    "Type the network name and password, or choose Scan for networks and pick one.",
    "Up/Down or Tab: move. Backspace: delete. Enter: select.",
    "Save keeps the network in picomultiloader.cfg on the SD card. Forget removes it.",
    "2.4 GHz networks only. Passwords are 8-64 characters, or empty for an open network.",
    "Wi-Fi autostart: ON starts the web file manager in the background at switch-on, as if you pressed F4 and then Esc. OFF (the default) waits for F4.",
#else
    "Up/Down: move. Enter: select. Settings are kept in picomultiloader.cfg on the SD card.",
#endif
    "Start folder: the folder the file list opens in at switch-on. Choose one, or Remember the last folder (the default).",
    "",
#if PICOMULTILOADER_WIFI
    "#Web file manager (F4)",
    "Joins your saved network and shows the address to open in a browser. http://" WEB_HOSTNAME ".local/ often works too.",
    "With no network saved, or if joining fails within 20 s, it makes its own network PicoMultiLoader-XXXX. Its password and the address http://192.168.4.1/ are shown on screen.",
    "In the browser: upload (drag and drop works), download, rename, delete, new folder, and Flash a .uf2 straight away.",
    "There is no login: use it on trusted networks only.",
    "Esc goes back to the files and keeps the server running. A Wi-Fi icon at the top shows it is on: green ready, blue starting, amber reconnecting, red failed. The file list refreshes when the browser changes files.",
    "F4 shows the server screen again; F4 there stops it. Flashing a file or USB drive mode stops it too.",
    "",
#endif
    "#USB drive mode (F5)",
    "Shows the SD card on a PC as a USB drive. Needs a cable on the Pico's micro-USB port; the PicoCalc's USB-C port is serial only.",
    "Eject the drive on the PC when done: the loader takes the card back by itself. Esc also leaves, but only press it after ejecting.",
#if PICOMULTILOADER_WIFI
    "Starting USB drive mode stops the web server.",
#endif
    "",
    "#Serial console",
    "115200 baud on the PicoCalc's USB-C port. Arrows, Enter, Backspace, Esc, F1, F4, F5 and typing work like the PicoCalc keyboard.",
    "",
    "#This help",
    "Up/Down: scroll. PgUp/PgDn: page. Home/End: top/bottom. Esc: back.",
};

#define HELP_MAX_LINES 200
typedef struct {
    const char *text;
    uint8_t len;
    bool heading;
} help_line_t;

static help_line_t s_help[HELP_MAX_LINES];
static int s_help_count, s_help_top;

static void help_add(const char *text, size_t len, bool heading)
{
    if (s_help_count < HELP_MAX_LINES) {
        s_help[s_help_count++] = (help_line_t){text, (uint8_t)len, heading};
    }
}

// Wrap HELP_TEXT into screen lines (once).
static void help_layout(void)
{
    if (s_help_count) {
        return;
    }
    for (size_t i = 0; i < sizeof(HELP_TEXT) / sizeof(HELP_TEXT[0]); i++) {
        const char *t = HELP_TEXT[i];
        bool heading = t[0] == '#';
        t += heading;
        size_t len = strlen(t);
        if (!len) {
            help_add("", 0, false);
        }
        for (size_t pos = 0; pos < len;) {
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
            help_add(t + pos, n, heading);
            pos += n;
            while (t[pos] == ' ') {
                pos++;
            }
        }
    }
}

static void enter_help(void)
{
    help_layout();
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
    for (int r = 0; r < LIST_ROWS && s_help_top + r < s_help_count; r++) {
        const help_line_t *l = &s_help[s_help_top + r];
        char line[COLS + 1];
        memcpy(line, l->text, l->len);
        line[l->len] = '\0';
        lcd_text(MARGIN, LIST_Y + r * ROW_H, line, l->heading ? C_ACCENT : C_FG, FONT_LARGE);
    }
    if (max_top) { // scroll bar
        int track = LIST_ROWS * ROW_H;
        int h = track * LIST_ROWS / s_help_count;
        int y = LIST_Y + (track - h) * s_help_top / max_top;
        lcd_fill_rect(LCD_W - 3, y - 1, 2, h, C_DIM);
    }
    char where[32];
    int last = s_help_top + LIST_ROWS < s_help_count ? s_help_top + LIST_ROWS : s_help_count;
    snprintf(where, sizeof(where), "Help  line %d of %d", last, s_help_count);
    draw_footer(where, C_DIM, "Up/Down scroll  PgUp/PgDn page  Esc back");
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
#define MAX_PICK 128

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
    draw_footer("Choose the folder to open at switch-on", C_DIM, "Enter open/choose  Left up  Esc cancel");
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
            snprintf(msg, sizeof(msg), "Opens %.40s at switch-on.", name);
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
    draw_option(y, "Help: how to use the loader", s_wf_row == WF_HELP);
    y += WF_ROW_H + 4;
    lcd_text(MARGIN, y, s_wf_msg, s_wf_msg_color, FONT_LARGE);
    y += ROW_H + 2;
    draw_wrapped(y, s_wf_row <= WF_PASS    ? "Type to edit, Backspace deletes. 2.4 GHz networks only."
                    : s_wf_row == WF_HELP ? "Keys, flashing, Wi-Fi, web and USB drive mode explained."
                    : s_wf_row == WF_START ? "The folder the file list opens in when you switch on."
                    : s_wf_row == WF_AUTO  ? "ON: the web file manager starts by itself at switch-on."
                                          : "F4 joins this network and starts the web file manager.",
                 C_DIM);
    draw_footer(NULL, C_FG, s_wf_dirty ? "Up/Down move  Enter select  Esc discard"
                                       : "Up/Down move  Enter select  Esc back");
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
    draw_footer(NULL, C_FG, s_ap_count ? "Up/Down select  Enter choose  Esc back" : "Esc: back");
}

static void wifi_scan(void)
{
    if (web_running()) {
        wf_message("Stop the web server (F4) to scan.", C_ERR);
        return;
    }
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
    draw_wrapped(y, s_wf_row == WF_HELP ? "Keys, flashing and USB drive mode explained."
                                        : "The folder the file list opens in when you switch on.",
                 C_DIM);
    draw_footer(NULL, C_FG, "Up/Down move  Enter select  Esc back");
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
        return true;
    case KEY_DOWN:
    case KEY_TAB:
        s_wf_row = (s_wf_row + 1) % WF_ROWS;
        return true;
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
    case KEY_F1:
        s_screen = SCREEN_BROWSE;
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

static bool inspect_file(void);
static void flash_and_run(void);

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
        strlcpy(s_sel_path, path, sizeof(s_sel_path));
        strlcpy(s_sel_name, strrchr(path, '/') + 1, sizeof(s_sel_name));
        if (inspect_file()) {
            flash_and_run(); // reboots on success, else shows the error screen
        } else {
            ui_redraw();
        }
        return;
    }
    if (s_screen == SCREEN_WEB) {
        web_status_t st;
        // redraw on changes, at most 4 times a second (uploads update the progress a lot)
        if (webserver_status(&st) != s_web_rev_shown && time_us_64() - s_web_drawn_us > 250000) {
            ui_redraw();
        }
        return;
    }
    uint32_t changes = webserver_changes();
    if (s_screen == SCREEN_BROWSE && changes != s_web_changes_seen) {
        s_web_changes_seen = changes;
        rescan_dir();
        ui_redraw();
    } else if (web_icon_color() != s_icon_shown) {
        ui_redraw();
    }
}
#endif

void ui_tick(void)
{
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
    switch (s_screen) {
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
    lcd_flush();
}

// ---- flashing ---------------------------------------------------------------------------------
#define BAR_X MARGIN
#define BAR_Y 170
#define BAR_W (LCD_W - 2 * MARGIN)
#define BAR_H 16

static void draw_progress(size_t done, size_t total)
{
    static int last = -1;
    int pct = total ? (int)((uint64_t)done * 100 / total) : 0;
    if (pct == last || (pct % 2 && done != total)) {
        return;
    }
    last = done == total ? -1 : pct;

    lcd_fill_rect(BAR_X, BAR_Y, BAR_W, BAR_H, C_FG);
    lcd_fill_rect(BAR_X + 1, BAR_Y + 1, BAR_W - 2, BAR_H - 2, C_BG);
    lcd_fill_rect(BAR_X + 2, BAR_Y + 2, (BAR_W - 4) * pct / 100, BAR_H - 4, C_ACCENT);
    char txt[16];
    snprintf(txt, sizeof(txt), "%d%%", pct);
    lcd_fill_rect(0, BAR_Y + BAR_H + 6, LCD_W, 14, C_BG);
    lcd_text((LCD_W - 7 * (int)strlen(txt)) / 2, BAR_Y + BAR_H + 6, txt, C_FG, FONT_LARGE);
    lcd_flush_rows(BAR_Y, BAR_H + 6 + 14);
}

static void flash_and_run(void)
{
#if PICOMULTILOADER_WIFI
    if (web_running()) {
        webserver_stop(); // nothing should touch the card or the radio while flashing
    }
#endif
    char err[96];
    if (s_info.ram_app) {
        save_setting("last_file", s_sel_name);
        draw_header();
        lcd_text(MARGIN, LIST_Y + 4, "Starting RAM app...", C_ACCENT, FONT_LARGE);
        draw_kv(LIST_Y + 4 + 2 * ROW_H, "File", s_sel_name);
        draw_footer(NULL, C_FG, "The UF2 Loader loads it from the card");
        lcd_flush();
        sleep_ms(200);
        uf2_run_ram_app(s_sel_path, err, sizeof(err)); // reboots on success
        snprintf(s_error, sizeof(s_error), "%s", err);
        s_screen = SCREEN_ERROR;
        ui_redraw();
        return;
    }

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

    lcd_text(MARGIN, BAR_Y + BAR_H + 30, "Done. Starting app...", C_OK, FONT_LARGE);
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
    s_screen = SCREEN_CONFIRM;
    return true;
}

static void open_selected(void)
{
    if (!s_sd_ok) {
        sdcard_unmount();
        s_sd_ok = sdcard_mount();
        config_reload();
        strlcpy(s_cwd, SDCARD_MOUNT_POINT, sizeof(s_cwd));
        scan_dir();
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
    inspect_file();
}

void ui_key(int key)
{
    switch (s_screen) {
    case SCREEN_BROWSE: {
        int n = item_count();
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
            change_dir(NULL);
            break;
        case KEY_F1:
            enter_wifi();
            break;
#if PICOMULTILOADER_WIFI
        case KEY_F4:
            if (web_running()) {
                s_screen = SCREEN_WEB; // already on in the background
            } else {
                enter_web();
            }
            break;
#endif
        case KEY_F5:
            enter_usb();
            break;
        default: // Esc does nothing here: the installed app only starts after a flash or a restart
            return;
        }
        break;
    }
    case SCREEN_CONFIRM:
        if (key == KEY_ENTER) {
            flash_and_run();
            return;
        }
        if (key == KEY_ESC || key == KEY_BACKSPACE || key == KEY_LEFT) {
            s_screen = SCREEN_BROWSE;
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
            lcd_fill_rect(0, FOOTER_Y + 1, LCD_W, LCD_H - FOOTER_Y - 1, C_BG);
            lcd_text(MARGIN, FOOTER_Y + 19, "Stopping...", C_FG, FONT_SMALL);
            lcd_flush();
            stop_web();
            break;
        }
        if (key == KEY_ESC || key == KEY_BACKSPACE || key == KEY_LEFT) {
            s_screen = SCREEN_BROWSE; // the server keeps running
            break;
        }
        return;
    }
#else
    case SCREEN_WEB:
    case SCREEN_WIFI_SCAN:
        s_screen = SCREEN_BROWSE;
        break;
#endif
    case SCREEN_ERROR:
        if (key == KEY_ESC || key == KEY_ENTER || key == KEY_BACKSPACE || key == KEY_LEFT) {
            s_screen = SCREEN_BROWSE;
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

void ui_init(bool sd_ok)
{
    lcd_set_palette(C_BG, RGB565(18, 20, 28));
    lcd_set_palette(C_FG, RGB565(235, 235, 235));
    lcd_set_palette(C_DIM, RGB565(140, 145, 160));
    lcd_set_palette(C_ACCENT, RGB565(120, 200, 255));
    lcd_set_palette(C_SEL_BG, RGB565(215, 215, 215));
    lcd_set_palette(C_SEL_FG, RGB565(10, 10, 10));
    lcd_set_palette(C_ERR, RGB565(255, 100, 100));
    lcd_set_palette(C_OK, RGB565(110, 220, 110));
    lcd_set_palette(C_WARN, RGB565(255, 190, 80));
    lcd_set_palette(C_FIELD, RGB565(40, 44, 58));
    lcd_set_palette(C_SIG_OFF, RGB565(70, 74, 90));

    s_sd_ok = sd_ok;
    s_screen = SCREEN_BROWSE;
    strlcpy(s_cwd, SDCARD_MOUNT_POINT, sizeof(s_cwd));

    // Open the start folder from Setup, else the last directory, else the root
    char last[PATH_MAX_LEN];
    if (!load_setting("start_dir", s_start_dir, sizeof(s_start_dir))) {
        s_start_dir[0] = '\0';
    }
    if (s_start_dir[0] && is_dir(s_start_dir)) {
        copy_dir(s_cwd, sizeof(s_cwd), s_start_dir);
    } else if (load_setting("last_dir", last, sizeof(last)) && last[0] == '/' && is_dir(last)) {
        copy_dir(s_cwd, sizeof(s_cwd), last);
    }
    printf("ui: opening %s\n", sd_path(s_cwd));
    scan_dir();

    // Preselect the file that was flashed last
    char last_file[64];
    if (load_setting("last_file", last_file, sizeof(last_file))) {
        for (int i = 0; i < item_count(); i++) {
            entry_t *e = item_entry(i);
            if (e && !e->is_dir && strcmp(e->name, last_file) == 0) {
                s_sel = i;
                break;
            }
        }
    }

#if PICOMULTILOADER_WIFI
    // Wi-Fi autostart: bring the web server up in the background (the top icon shows its state)
    char web_auto[4];
    s_web_auto = load_setting("web_auto", web_auto, sizeof(web_auto)) && web_auto[0] == '1';
    if (s_web_auto && sd_ok) {
        s_web_changes_seen = webserver_changes();
        if (!webserver_start()) {
            printf("ui: Wi-Fi autostart failed\n");
        }
    }
#endif
    ui_redraw();
}
