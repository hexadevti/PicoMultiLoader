// pmlapp.c - the bundled apps' shared code (pmlapp.h).
#include "pmlapp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "board.h"
#include "config.h"

#define KBD_EVERY_US     15000              // the keyboard's I2C read takes ~7 ms at 10 kHz
#define BATTERY_EVERY_US (10 * 1000 * 1000)
#define REPROBE_EVERY_US (1000 * 1000)

#define SETTINGS_FILE APP_DIR "/settings.ini"
#define SETTINGS_MAX  16
#define SETTING_LEN   96

static bool s_sd_ok;
static void (*s_idle)(void);
static uint64_t s_next_kbd, s_next_battery, s_next_probe;
static int s_battery = -1;
static bool s_charging;

// ---- start-up -----------------------------------------------------------------------------------
void app_init(void)
{
    stdio_uart_init_full(CONSOLE_UART, CONSOLE_BAUD, CONSOLE_TX_PIN, CONSOLE_RX_PIN);
    printf("\n%s\n", APP_NAME);
    s_sd_ok = sdcard_mount();
    lcd_init();
    char theme[32];
    theme_load(s_sd_ok && config_get("theme", theme, sizeof(theme)) ? theme : NULL);
    lcd_fill_rect(0, 0, LCD_W, LCD_H, C_BG);
    lcd_flush();
    if (kbd_init()) {
        s_battery = kbd_battery(&s_charging);
    } else {
        printf("%s: no keyboard - is the PicoCalc switched on? Serial input still works.\n", APP_NAME);
    }
    kbd_repeat_more(true);
    s_next_battery = time_us_64() + BATTERY_EVERY_US;
}

bool app_sd_ok(void)
{
    return s_sd_ok;
}

int app_battery(bool *charging)
{
    if (charging) {
        *charging = s_charging;
    }
    return s_battery;
}

// ---- keys ---------------------------------------------------------------------------------------
static int console_getc(uint32_t wait_us)
{
    int c = getchar_timeout_us(wait_us);
    return c == PICO_ERROR_TIMEOUT ? -1 : c;
}

// Terminal input (ANSI escape sequences) as PicoCalc key codes, as the loader reads it.
static int console_key(void)
{
    int c = console_getc(0);
    switch (c) {
    case -1:
        return KEY_NONE;
    case '\r':
    case '\n':
        return KEY_ENTER;
    case 0x08:
    case 0x7F:
        return KEY_BACKSPACE;
    case '\t':
        return KEY_TAB;
    case 0x10: // Ctrl+P: dump the screen for screenshots
        lcd_dump();
        return KEY_NONE;
    case 0x1B:
        break;
    default:
        return (c >= 0x20 && c < 0x7F) || (c > 0 && c < 0x20) ? c : KEY_NONE; // text and Ctrl+letter
    }

    int c1 = console_getc(20000);
    if (c1 != '[' && c1 != 'O') {
        return KEY_ESC;
    }
    int c2 = console_getc(20000);
    switch (c2) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    case 'P': return KEY_F1;
    case 'Q': return KEY_F2;
    case 'R': return KEY_F3;
    case 'S': return KEY_F4;
    case 'T': return KEY_F5;
    default:
        break;
    }
    int num = 0; // ESC [ <number> ~
    for (int k = c2; k != '~'; k = console_getc(20000)) {
        if (k < '0' || k > '9' || num > 99) {
            return KEY_NONE;
        }
        num = num * 10 + (k - '0');
    }
    switch (num) {
    case 1: case 7: return KEY_HOME;
    case 4: case 8: return KEY_END;
    case 3: return KEY_DEL;
    case 5: return KEY_PAGE_UP;
    case 6: return KEY_PAGE_DOWN;
    case 11: return KEY_F1;
    case 12: return KEY_F2;
    case 13: return KEY_F3;
    case 14: return KEY_F4;
    case 15: return KEY_F5;
    default: return KEY_NONE;
    }
}

void app_set_idle(void (*fn)(void))
{
    s_idle = fn;
}

void app_idle(void)
{
    if (s_idle) {
        s_idle();
    }
    look_tick();
    uint64_t now = time_us_64();
    if (now >= s_next_battery && kbd_present()) {
        s_next_battery = now + BATTERY_EVERY_US;
        s_battery = kbd_battery(&s_charging);
    }
}

int app_poll_key(void)
{
    app_idle();
    int key = console_key();
    if (key != KEY_NONE) {
        return key;
    }
    uint64_t now = time_us_64();
    if (now < s_next_kbd) {
        return KEY_NONE;
    }
    s_next_kbd = now + KBD_EVERY_US;
    if (kbd_present()) {
        key = kbd_poll();
        if (key == KEY_F10_ALT) {
            key = KEY_F10;
        }
        if (key != KEY_NONE) {
            s_next_kbd = 0; // more may be queued
        }
        return key;
    }
    if (now >= s_next_probe) {
        // The PicoCalc was switched on after us: the panel lost power, so set it up again.
        s_next_probe = now + REPROBE_EVERY_US;
        if (kbd_init()) {
            lcd_init();
            lcd_flush();
        }
    }
    return KEY_NONE;
}

int app_wait_key(void)
{
    for (;;) {
        int key = app_poll_key();
        if (key != KEY_NONE) {
            return key;
        }
        sleep_ms(2);
    }
}

// ---- the frame ----------------------------------------------------------------------------------
int app_text_clip(int x, int y, const char *s, int max_chars, lcd_color_t color, lcd_font_t font)
{
    if (max_chars <= 0) {
        return x;
    }
    if ((int)strlen(s) <= max_chars) {
        return lcd_text(x, y, s, color, font);
    }
    char buf[128];
    int n = max_chars - 1 < (int)sizeof(buf) - 2 ? max_chars - 1 : (int)sizeof(buf) - 2;
    memcpy(buf, s, n);
    buf[n] = '~';
    buf[n + 1] = '\0';
    return lcd_text(x, y, buf, color, font);
}

void app_header(const char *title, const char *right)
{
    lcd_fill_rect(0, 0, LCD_W, APP_HEADER_H - 1, C_PANEL);
    lcd_hline(0, APP_HEADER_H - 1, LCD_W, C_LINE);
    lcd_fill_rect(0, 0, 3, APP_HEADER_H - 1, C_ACCENT);
    int x = LCD_W - 4;
    if (s_battery >= 0) {
        char b[16];
        snprintf(b, sizeof(b), "%s%d%%", s_charging ? "+" : "", s_battery);
        x -= (int)strlen(b) * 6;
        lcd_text(x, 5, b, s_battery <= 15 && !s_charging ? C_ERR : C_DIM, FONT_SMALL);
        x -= 8;
    }
    if (right && right[0]) {
        x -= (int)strlen(right) * 6;
        lcd_text(x, 5, right, C_FG, FONT_SMALL);
        x -= 8;
    }
    app_text_clip(8, 3, title, (x - 8) / 7, C_BRIGHT, FONT_LARGE);
}

void app_footer(const char *keys)
{
    lcd_fill_rect(0, APP_BOTTOM, LCD_W, APP_FOOTER_H, C_PANEL);
    lcd_hline(0, APP_BOTTOM, LCD_W, C_LINE);
    // Items are separated by two spaces; each one's first word is the key.
    int x = 4;
    const char *s = keys;
    while (*s && x < LCD_W) {
        while (*s == ' ') {
            s++;
        }
        const char *end = strstr(s, "  ");
        size_t n = end ? (size_t)(end - s) : strlen(s);
        char item[48];
        if (n >= sizeof(item)) {
            n = sizeof(item) - 1;
        }
        memcpy(item, s, n);
        item[n] = '\0';
        char *sp = strchr(item, ' ');
        if (sp) {
            *sp = '\0';
        }
        x = lcd_text(x, APP_BOTTOM + 3, item, C_ACCENT, FONT_SMALL);
        if (sp) {
            x = lcd_text(x + 3, APP_BOTTOM + 3, sp + 1, C_FG, FONT_SMALL);
        }
        x += 9;
        s += n;
    }
}

void app_clear_body(void)
{
    lcd_fill_rect(0, APP_TOP, LCD_W, APP_BODY_H, C_BG);
}

void app_toast(const char *text)
{
    lcd_fill_rect(0, APP_BOTTOM + 1, LCD_W, APP_FOOTER_H - 1, C_FIELD);
    app_text_clip(4, APP_BOTTOM + 3, text, (LCD_W - 8) / 6, C_WARN, FONT_SMALL);
    lcd_flush_rows(APP_BOTTOM, APP_FOOTER_H);
}

// ---- dialogs ------------------------------------------------------------------------------------
#define DLG_X    16
#define DLG_W    (LCD_W - 2 * DLG_X)
#define DLG_COLS ((DLG_W - 16) / 6)
#define DLG_MAX_LINES 14

// Greedy word wrap into lines of up to `cols` characters; '\n' breaks a line.
static int wrap(const char *text, int cols, char lines[][DLG_COLS + 1], int max_lines)
{
    int n = 0;
    const char *s = text ? text : "";
    while (*s && n < max_lines) {
        int len = 0, cut = -1;
        while (s[len] && s[len] != '\n' && len < cols) {
            if (s[len] == ' ') {
                cut = len;
            }
            len++;
        }
        if (s[len] && s[len] != '\n' && s[len] != ' ' && cut > 0) {
            len = cut; // break at the last space
        }
        memcpy(lines[n], s, len);
        lines[n][len] = '\0';
        n++;
        s += len;
        if (*s == '\n' || *s == ' ') {
            s++;
        }
    }
    return n;
}

// The dialog box: the screen dimmed behind it, a title bar. Returns the y of the first text line.
static int dialog_box(int y, int h, const char *title)
{
    look_dither(0, LCD_H);
    lcd_fill_rect(DLG_X, y, DLG_W, h, C_PANEL);
    look_frame(DLG_X, y, DLG_W, h, C_ACCENT);
    lcd_fill_rect(DLG_X + 1, y + 1, DLG_W - 2, 16, C_ACCDK);
    app_text_clip(DLG_X + 7, y + 3, title, DLG_COLS, C_BRIGHT, FONT_SMALL);
    return y + 23;
}

static int dialog_text(const char *title, const char *text, const char *keys, int extra_h)
{
    char lines[DLG_MAX_LINES][DLG_COLS + 1];
    int n = wrap(text, DLG_COLS, lines, DLG_MAX_LINES);
    int h = 23 + n * 11 + extra_h + 22;
    int y = (LCD_H - h) / 2;
    int ty = dialog_box(y, h, title);
    for (int i = 0; i < n; i++) {
        lcd_text(DLG_X + 8, ty + i * 11, lines[i], C_FG, FONT_SMALL);
    }
    lcd_text(DLG_X + 8, y + h - 15, keys, C_DIM, FONT_SMALL);
    return ty + n * 11;
}

void app_message(const char *title, const char *text)
{
    dialog_text(title, text, "Press any key", 0);
    lcd_flush();
    app_wait_key();
}

int app_ask(const char *title, const char *text)
{
    dialog_text(title, text, "Y / Enter: yes   N: no   Esc: cancel", 0);
    lcd_flush();
    for (;;) {
        int k = app_wait_key();
        if (k == 'y' || k == 'Y' || k == KEY_ENTER) {
            return 1;
        }
        if (k == 'n' || k == 'N') {
            return 0;
        }
        if (k == KEY_ESC) {
            return -1;
        }
    }
}

bool app_confirm(const char *title, const char *text)
{
    dialog_text(title, text, "Y / Enter: yes   N / Esc: no", 0);
    lcd_flush();
    for (;;) {
        int k = app_wait_key();
        if (k == 'y' || k == 'Y' || k == KEY_ENTER) {
            return true;
        }
        if (k == 'n' || k == 'N' || k == KEY_ESC) {
            return false;
        }
    }
}

bool app_input(const char *title, const char *prompt, char *buf, size_t len)
{
    const int field_cols = DLG_COLS - 2;
    int cur = (int)strlen(buf), left = 0;
    for (;;) {
        int fy = dialog_text(title, prompt, "Enter: OK   Esc: cancel", 20) + 4;
        int n = (int)strlen(buf);
        if (cur < left) {
            left = cur;
        }
        if (cur - left >= field_cols) {
            left = cur - field_cols + 1;
        }
        lcd_fill_rect(DLG_X + 6, fy, DLG_W - 12, 15, C_FIELD);
        look_frame(DLG_X + 6, fy, DLG_W - 12, 15, C_LINE);
        char vis[DLG_COLS + 1];
        int vn = n - left < field_cols ? n - left : field_cols;
        memcpy(vis, buf + left, vn);
        vis[vn] = '\0';
        lcd_text(DLG_X + 10, fy + 3, vis, C_BRIGHT, FONT_SMALL);
        lcd_fill_rect(DLG_X + 10 + (cur - left) * 6, fy + 2, 1, 11, C_ACCENT);
        lcd_flush();

        int k = app_wait_key();
        if (k == KEY_ENTER) {
            return true;
        } else if (k == KEY_ESC) {
            return false;
        } else if (k == KEY_LEFT && cur > 0) {
            cur--;
        } else if (k == KEY_RIGHT && cur < n) {
            cur++;
        } else if (k == KEY_HOME) {
            cur = 0;
        } else if (k == KEY_END) {
            cur = n;
        } else if (k == KEY_BACKSPACE && cur > 0) {
            memmove(buf + cur - 1, buf + cur, n - cur + 1);
            cur--;
        } else if (k == KEY_DEL && cur < n) {
            memmove(buf + cur, buf + cur + 1, n - cur);
        } else if (k >= 0x20 && k < 0x7F && n + 1 < (int)len) {
            memmove(buf + cur + 1, buf + cur, n - cur + 1);
            buf[cur++] = (char)k;
        }
    }
}

void app_help(const char *title, const char *const *lines, int count)
{
    const int rows = APP_BODY_H / 11 - 1;
    int top = 0;
    for (;;) {
        app_header(title, NULL);
        app_clear_body();
        for (int i = 0; i < rows && top + i < count; i++) {
            const char *s = lines[top + i];
            // "Key: text" lines show the key part in the accent colour
            const char *colon = strstr(s, "  ");
            int y = APP_TOP + 6 + i * 11;
            if (colon && s[0] != ' ') {
                char key[24];
                size_t n = (size_t)(colon - s) < sizeof(key) - 1 ? (size_t)(colon - s) : sizeof(key) - 1;
                memcpy(key, s, n);
                key[n] = '\0';
                lcd_text(8, y, key, C_ACCENT, FONT_SMALL);
                app_text_clip(8 + 6 * 11, y, colon + 2, (LCD_W - 16) / 6 - 11, C_FG, FONT_SMALL);
            } else {
                app_text_clip(8, y, s, (LCD_W - 16) / 6, s[0] == '#' ? C_BRIGHT : C_FG, FONT_SMALL);
            }
        }
        app_footer(count > rows ? "Up/Down Scroll  Esc Close" : "Esc Close");
        lcd_flush();
        int k = app_wait_key();
        if (k == KEY_DOWN && top + rows < count) {
            top++;
        } else if (k == KEY_PAGE_DOWN && top + rows < count) {
            top = top + rows < count - rows ? top + rows : count - rows;
        } else if (k == KEY_UP && top > 0) {
            top--;
        } else if (k == KEY_PAGE_UP) {
            top = top > rows ? top - rows : 0;
        } else if (k != KEY_UP && k != KEY_DOWN && k != KEY_PAGE_UP && k != KEY_PAGE_DOWN) {
            return;
        }
    }
}

// ---- files --------------------------------------------------------------------------------------
bool app_has_ext(const char *name, const char *exts)
{
    if (!exts || !exts[0]) {
        return true;
    }
    size_t nl = strlen(name);
    for (const char *e = exts; *e;) {
        size_t el = strcspn(e, ",");
        if (el && el <= nl && strncasecmp(name + nl - el, e, el) == 0) {
            return true;
        }
        e += el;
        if (*e == ',') {
            e++;
        }
    }
    return false;
}

void app_dirname(const char *path, char *out, size_t len)
{
    const char *slash = strrchr(path, '/');
    size_t n = slash ? (size_t)(slash - path) : 0;
    if (n >= len) {
        n = len - 1;
    }
    memcpy(out, path, n);
    out[n] = '\0';
}

const char *app_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

#define PICK_MAX  192
#define PICK_NAME 96

typedef struct {
    char name[PICK_NAME];
    bool dir;
} pick_entry_t;

static int pick_cmp(const void *a, const void *b)
{
    const pick_entry_t *x = a, *y = b;
    if (x->dir != y->dir) {
        return x->dir ? -1 : 1;
    }
    return strcasecmp(x->name, y->name);
}

// The folder's sub-folders and matching files, sorted. Returns the count.
static int pick_list(const char *dir, const char *exts, pick_entry_t *e)
{
    FF_DIR d;
    FILINFO fi;
    int n = 0;
    if (f_opendir(&d, sd_path(dir)) != FR_OK) {
        return 0;
    }
    while (n < PICK_MAX && f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        bool is_dir = (fi.fattrib & AM_DIR) != 0;
        if (fi.fname[0] == '.' || (fi.fattrib & (AM_HID | AM_SYS)) || strlen(fi.fname) >= PICK_NAME ||
            (!is_dir && !app_has_ext(fi.fname, exts))) {
            continue;
        }
        strcpy(e[n].name, fi.fname);
        e[n].dir = is_dir;
        n++;
    }
    f_closedir(&d);
    qsort(e, n, sizeof(e[0]), pick_cmp);
    return n;
}

bool app_pick_file(const char *title, const char *exts, char *path, size_t len)
{
    pick_entry_t *e = malloc(PICK_MAX * sizeof(pick_entry_t));
    if (!e) {
        app_message(title, "Not enough memory.");
        return false;
    }
    char dir[256], want[PICK_NAME] = "";
    FILINFO fi;
    strlcpy(dir, path, sizeof(dir));
    if (dir[0] && (f_stat(dir, &fi) != FR_OK || !(fi.fattrib & AM_DIR))) {
        strlcpy(want, app_basename(path), sizeof(want)); // a file: start in its folder, on it
        app_dirname(path, dir, sizeof(dir));
    }
    if (dir[0] && f_stat(dir, &fi) != FR_OK) {
        dir[0] = '\0';
    }
    if (strcmp(dir, "/") == 0) {
        dir[0] = '\0';
    }

    const int row_h = 12, list_y = APP_TOP + 16, rows = (APP_BOTTOM - list_y) / row_h;
    int n = pick_list(dir, exts, e), sel = 0, top = 0;
    bool ok = false;
    for (int i = 0; i < n; i++) {
        if (strcasecmp(e[i].name, want) == 0) {
            sel = i;
        }
    }
    for (;;) {
        if (sel >= n) {
            sel = n - 1;
        }
        if (sel < 0) {
            sel = 0;
        }
        if (sel < top) {
            top = sel;
        }
        if (sel >= top + rows) {
            top = sel - rows + 1;
        }
        app_header(title, NULL);
        app_clear_body();
        lcd_fill_rect(0, APP_TOP, LCD_W, 14, C_FIELD);
        app_text_clip(6, APP_TOP + 2, dir[0] ? dir : "/", (LCD_W - 12) / 6, C_DIM, FONT_SMALL);
        if (n == 0) {
            lcd_text(8, list_y + 6, exts && exts[0] ? "No matching files here." : "This folder is empty.", C_DIM, FONT_SMALL);
        }
        for (int i = 0; i < rows && top + i < n; i++) {
            int y = list_y + i * row_h, k = top + i;
            bool s = k == sel;
            if (s) {
                lcd_fill_rect(0, y, LCD_W, row_h, C_SEL_BG);
            }
            lcd_text(6, y + 1, e[k].dir ? ">" : " ", s ? C_SEL_FG : C_ACCENT, FONT_SMALL);
            app_text_clip(16, y + 1, e[k].name, (LCD_W - 24) / 6, s ? C_SEL_FG : e[k].dir ? C_BRIGHT : C_FG, FONT_SMALL);
        }
        app_footer("Enter Open  Bksp Up  Esc Cancel");
        lcd_flush();

        int k = app_wait_key();
        if (k == KEY_UP) {
            sel = sel > 0 ? sel - 1 : n - 1;
        } else if (k == KEY_DOWN) {
            sel = sel + 1 < n ? sel + 1 : 0;
        } else if (k == KEY_PAGE_UP) {
            sel -= rows;
        } else if (k == KEY_PAGE_DOWN) {
            sel += rows;
        } else if (k == KEY_HOME) {
            sel = 0;
        } else if (k == KEY_END) {
            sel = n - 1;
        } else if (k == KEY_ESC) {
            break;
        } else if ((k == KEY_BACKSPACE || k == KEY_LEFT) && dir[0]) {
            strlcpy(want, app_basename(dir), sizeof(want));
            char up[256];
            app_dirname(dir, up, sizeof(up));
            strlcpy(dir, up, sizeof(dir));
            n = pick_list(dir, exts, e);
            sel = top = 0;
            for (int i = 0; i < n; i++) {
                if (strcasecmp(e[i].name, want) == 0) {
                    sel = i;
                }
            }
        } else if ((k == KEY_ENTER || k == KEY_RIGHT) && n > 0) {
            if (e[sel].dir) {
                if (strlen(dir) + strlen(e[sel].name) + 2 < sizeof(dir)) {
                    strlcat(dir, "/", sizeof(dir));
                    strlcat(dir, e[sel].name, sizeof(dir));
                    n = pick_list(dir, exts, e);
                    sel = top = 0;
                }
            } else if (k == KEY_ENTER) {
                snprintf(path, len, "%s/%s", dir, e[sel].name);
                ok = true;
                break;
            }
        } else if (k > ' ' && k < 0x7F) {
            // jump to the next entry starting with the letter
            for (int i = 1; i <= n; i++) {
                int j = (sel + i) % n;
                if (strncasecmp(e[j].name, (char[]){(char)k, 0}, 1) == 0) {
                    sel = j;
                    break;
                }
            }
        }
    }
    free(e);
    return ok;
}

// ---- settings -----------------------------------------------------------------------------------
static char s_set[SETTINGS_MAX][SETTING_LEN]; // "key=value"
static int s_set_count = -1;

static void settings_load(void)
{
    if (s_set_count >= 0) {
        return;
    }
    s_set_count = 0;
    FIL f;
    if (!s_sd_ok || f_open(&f, SETTINGS_FILE, FA_READ) != FR_OK) {
        return;
    }
    char line[SETTING_LEN];
    while (s_set_count < SETTINGS_MAX && f_gets(line, sizeof(line), &f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strchr(line, '=') && line[0] != '=' && line[0] != ';') {
            strcpy(s_set[s_set_count++], line);
        }
    }
    f_close(&f);
}

static int settings_find(const char *key)
{
    size_t kl = strlen(key);
    for (int i = 0; i < s_set_count; i++) {
        if (strncmp(s_set[i], key, kl) == 0 && s_set[i][kl] == '=') {
            return i;
        }
    }
    return -1;
}

bool app_setting_get(const char *key, char *buf, size_t len)
{
    settings_load();
    int i = settings_find(key);
    if (i < 0) {
        return false;
    }
    strlcpy(buf, s_set[i] + strlen(key) + 1, len);
    return true;
}

void app_setting_set(const char *key, const char *value)
{
    settings_load();
    int i = settings_find(key);
    if (i < 0) {
        if (s_set_count >= SETTINGS_MAX) {
            return;
        }
        i = s_set_count++;
    }
    snprintf(s_set[i], SETTING_LEN, "%s=%s", key, value);
    FILINFO fi;
    FIL f;
    if (!s_sd_ok || f_stat(APP_DIR, &fi) != FR_OK || f_open(&f, SETTINGS_FILE, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        return;
    }
    for (int k = 0; k < s_set_count; k++) {
        f_printf(&f, "%s\n", s_set[k]);
    }
    f_close(&f);
}

// ---- leaving ------------------------------------------------------------------------------------
void app_exit(const char *message)
{
    FIL f;
    if (message && s_sd_ok && f_open(&f, "/pml/exit.txt", FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
        f_printf(&f, "%s\n", message);
        f_close(&f);
    }
    sdcard_unmount();
    lcd_fill_rect(0, 0, LCD_W, LCD_H, C_BG);
    lcd_flush();
    // The menu request uf2loader and PicoMultiLoader read (docs/apps.md)
    watchdog_hw->scratch[0] = 0xe98cc638;
    watchdog_hw->scratch[1] = 1;
    watchdog_reboot(0, 0, 0);
    for (;;) {
    }
}

// FatFs file times: the PicoCalc has no clock that survives starting an app.
DWORD get_fattime(void)
{
    return (DWORD)(2026 - 1980) << 25 | 1u << 21 | 1u << 16;
}

// sdcard.c's free-space counter borrows the loader's flashing buffer; the apps don't count.
void *uf2_scratch(size_t *len)
{
    static uint8_t buf[512];
    *len = sizeof(buf);
    return buf;
}
