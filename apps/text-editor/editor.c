// editor.c - a plain text editor: open, edit and save text files on the SD card.
//
// The text is a gap buffer (one array with a hole at the cursor, so typing is cheap). Lines end in
// '\n' in memory; a file that used CRLF is saved with CRLF again. Tabs show as 4-column stops and
// the Tab key inserts spaces. Ctrl+X / Ctrl+C / Ctrl+V cut, copy and paste whole lines: pressing
// Ctrl+X or Ctrl+C again adds the next line, so a block moves in a few presses.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "pico/stdlib.h"
#include "pmlapp.h"

#if PICO_RP2040
#define TEXT_MAX (96 * 1024)
#define TEXT_KB  "95 KB"
#else
#define TEXT_MAX (256 * 1024)
#define TEXT_KB  "255 KB"
#endif
#define CLIP_MAX (8 * 1024)
#define TAB      4

#define ROW_H    11
#define STATUS_H 13
#define ROWS     ((APP_BODY_H - STATUS_H - 2) / ROW_H)
#define GUTTER   5 // line number columns
#define TEXT_X   (GUTTER * 6 + 4)
#define COLS     ((LCD_W - TEXT_X - 2) / 6)

static char s_buf[TEXT_MAX];
static int s_gap0, s_gap1 = TEXT_MAX; // the gap is s_buf[s_gap0..s_gap1)
static int s_cur;                     // the cursor, as an offset in the text
static int s_top;                     // offset of the first line on screen
static int s_left;                    // first column on screen
static int s_want_col = -1;           // the column Up / Down aim for
static bool s_modified, s_crlf, s_bom;
static char s_path[256];
static char s_clip[CLIP_MAX];
static int s_clip_len;
static int s_last_key;
static char s_find[64];

// ---- the buffer ---------------------------------------------------------------------------------
static inline int text_len(void)
{
    return TEXT_MAX - (s_gap1 - s_gap0);
}

static inline char at(int i)
{
    return i < s_gap0 ? s_buf[i] : s_buf[i + (s_gap1 - s_gap0)];
}

static void move_gap(int pos)
{
    if (pos < s_gap0) {
        int n = s_gap0 - pos;
        memmove(&s_buf[s_gap1 - n], &s_buf[pos], n);
        s_gap0 -= n;
        s_gap1 -= n;
    } else if (pos > s_gap0) {
        int n = pos - s_gap0;
        memmove(&s_buf[s_gap0], &s_buf[s_gap1], n);
        s_gap0 += n;
        s_gap1 += n;
    }
}

static bool insert(int pos, const char *s, int n)
{
    if (n > s_gap1 - s_gap0) {
        app_toast("The file is full.");
        return false;
    }
    move_gap(pos);
    memcpy(&s_buf[s_gap0], s, n);
    s_gap0 += n;
    s_modified = true;
    return true;
}

static void erase(int pos, int n)
{
    if (n <= 0) {
        return;
    }
    move_gap(pos);
    s_gap1 += n;
    s_modified = true;
}

static void clear_text(void)
{
    s_gap0 = 0;
    s_gap1 = TEXT_MAX;
    s_cur = s_top = s_left = 0;
    s_want_col = -1;
    s_modified = s_crlf = s_bom = false;
}

// ---- lines --------------------------------------------------------------------------------------
static int line_start(int pos)
{
    while (pos > 0 && at(pos - 1) != '\n') {
        pos--;
    }
    return pos;
}

static int line_end(int pos)
{
    int n = text_len();
    while (pos < n && at(pos) != '\n') {
        pos++;
    }
    return pos;
}

// The start of the next line, or -1 on the last line.
static int next_line(int ls)
{
    int le = line_end(ls);
    return le < text_len() ? le + 1 : -1;
}

static int prev_line(int ls)
{
    return ls > 0 ? line_start(ls - 1) : -1;
}

static int col_of(int ls, int pos)
{
    int c = 0;
    for (int i = ls; i < pos; i++) {
        c = at(i) == '\t' ? (c / TAB + 1) * TAB : c + 1;
    }
    return c;
}

static int pos_at_col(int ls, int col)
{
    int le = line_end(ls), c = 0, i = ls;
    while (i < le) {
        int nc = at(i) == '\t' ? (c / TAB + 1) * TAB : c + 1;
        if (nc > col) {
            break;
        }
        c = nc;
        i++;
    }
    return i;
}

// 1-based line number of `pos`.
static int line_number(int pos)
{
    int n = 1;
    for (int i = 0; i < pos; i++) {
        n += at(i) == '\n';
    }
    return n;
}

static int line_count(void)
{
    return line_number(text_len());
}

// Keep the cursor on screen (scrolling by lines), with `centre` after a jump.
static void scroll_to_cursor(bool centre)
{
    int cl = line_start(s_cur);
    if (cl < s_top) {
        s_top = cl;
    } else {
        int ls = s_top, row = 0;
        while (ls != cl && ls >= 0 && row < ROWS) {
            ls = next_line(ls);
            row++;
        }
        if (row >= ROWS || ls != cl) {
            s_top = cl;
            for (int i = 0; i < (centre ? ROWS / 2 : ROWS - 1) && s_top > 0; i++) {
                s_top = prev_line(s_top);
            }
        }
    }
    if (centre && cl == s_top) {
        for (int i = 0; i < ROWS / 2 && s_top > 0; i++) {
            s_top = prev_line(s_top);
        }
    }
    int c = col_of(cl, s_cur);
    if (c < s_left) {
        s_left = c > 8 ? c - 8 : 0;
    } else if (c >= s_left + COLS) {
        s_left = c - COLS + 9;
    }
}

// ---- files --------------------------------------------------------------------------------------
static bool load(const char *path)
{
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) {
        app_message("Open", "Can't open the file.");
        return false;
    }
    FSIZE_t size = f_size(&f);
    if (size > TEXT_MAX - 1024) {
        f_close(&f);
        char msg[96];
        snprintf(msg, sizeof(msg), "The file is too big: this editor holds up to %d KB.", (TEXT_MAX - 1024) / 1024);
        app_message("Open", msg);
        return false;
    }
    clear_text();
    UINT n = 0;
    bool ok = f_read(&f, s_buf, (UINT)size, &n) == FR_OK && n == size;
    f_close(&f);
    if (!ok) {
        app_message("Open", "Read error.");
        n = 0;
    }
    // Drop a UTF-8 BOM and the '\r' of CRLF line ends (both come back on saving).
    int start = 0;
    if (n >= 3 && (uint8_t)s_buf[0] == 0xEF && (uint8_t)s_buf[1] == 0xBB && (uint8_t)s_buf[2] == 0xBF) {
        s_bom = true;
        start = 3;
    }
    int w = 0;
    for (int r = start; r < (int)n; r++) {
        if (s_buf[r] == '\r' && r + 1 < (int)n && s_buf[r + 1] == '\n') {
            s_crlf = true;
            continue;
        }
        s_buf[w++] = s_buf[r];
    }
    s_gap0 = w;
    s_gap1 = TEXT_MAX;
    s_modified = false;
    strlcpy(s_path, path, sizeof(s_path));
    app_setting_set("last", s_path);
    return ok;
}

static bool write_all(FIL *f)
{
    char out[512];
    int o = 0, n = text_len();
    UINT wn;
    if (s_bom && f_write(f, "\xEF\xBB\xBF", 3, &wn) != FR_OK) {
        return false;
    }
    for (int i = 0; i < n; i++) {
        char c = at(i);
        if (c == '\n' && s_crlf) {
            out[o++] = '\r';
        }
        out[o++] = c;
        if (o >= (int)sizeof(out) - 1) {
            if (f_write(f, out, o, &wn) != FR_OK || wn != (UINT)o) {
                return false;
            }
            o = 0;
        }
    }
    return o == 0 || (f_write(f, out, o, &wn) == FR_OK && wn == (UINT)o);
}

// Write to "<path>.tmp", then put it in place, so a failed save keeps the old file.
static bool save_to(const char *path)
{
    char tmp[sizeof(s_path) + 4];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FIL f;
    if (f_open(&f, tmp, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        app_message("Save", "Can't create the file. Is the folder there, and the card not locked?");
        return false;
    }
    bool ok = write_all(&f);
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
    s_modified = false;
    app_setting_set("last", s_path);
    return true;
}

static bool save_as(void)
{
    char path[sizeof(s_path)];
    if (s_path[0]) {
        strlcpy(path, s_path, sizeof(path));
    } else {
        strlcpy(path, "/untitled.txt", sizeof(path));
    }
    if (!app_input("Save as", "Path of the file on the card:", path, sizeof(path)) || !path[0]) {
        return false;
    }
    if (path[0] != '/') {
        memmove(path + 1, path, strlen(path) + 1 < sizeof(path) - 1 ? strlen(path) + 1 : sizeof(path) - 2);
        path[0] = '/';
    }
    FILINFO fi;
    if (strcmp(path, s_path) != 0 && f_stat(path, &fi) == FR_OK &&
        !app_confirm("Save as", "That file exists. Replace it?")) {
        return false;
    }
    return save_to(path);
}

static bool save(void)
{
    return s_path[0] ? save_to(s_path) : save_as();
}

// Unsaved changes: ask. False if the user cancelled.
static bool may_discard(void)
{
    if (!s_modified) {
        return true;
    }
    int a = app_ask("Unsaved changes", "Save the changes first?");
    return a == 0 || (a == 1 && save());
}

static void open_file(void)
{
    if (!may_discard()) {
        return;
    }
    char path[sizeof(s_path)];
    strlcpy(path, s_path[0] ? s_path : "/", sizeof(path));
    if (app_pick_file("Open a file", "", path, sizeof(path))) {
        if (!load(path)) {
            clear_text();
            s_path[0] = '\0';
        }
    }
}

// ---- editing ------------------------------------------------------------------------------------
static void type_newline(void)
{
    // keep the indentation of the line
    char ind[64];
    int ls = line_start(s_cur), n = 0;
    ind[n++] = '\n';
    for (int i = ls; i < s_cur && n < (int)sizeof(ind) && (at(i) == ' ' || at(i) == '\t'); i++) {
        ind[n++] = at(i);
    }
    if (insert(s_cur, ind, n)) {
        s_cur += n;
    }
}

static void type_tab(void)
{
    int c = col_of(line_start(s_cur), s_cur);
    int n = TAB - c % TAB;
    if (insert(s_cur, "        ", n)) {
        s_cur += n;
    }
}

// Ctrl+X / Ctrl+C: the line (with its '\n') to the clipboard, added to it if the last key was the same.
static void clip_line(bool cut, bool append)
{
    int ls = line_start(s_cur), next = next_line(ls);
    int le = next < 0 ? text_len() : next;
    if (!append) {
        s_clip_len = 0;
    }
    int n = le - ls;
    if (s_clip_len + n + 1 > CLIP_MAX) {
        app_toast("The clipboard is full.");
        return;
    }
    for (int i = 0; i < n; i++) {
        s_clip[s_clip_len++] = at(ls + i);
    }
    if (next < 0) {
        s_clip[s_clip_len++] = '\n'; // the last line has none
    }
    if (cut) {
        erase(ls, n);
        s_cur = ls > text_len() ? text_len() : ls;
    } else {
        s_cur = next < 0 ? line_end(ls) : next;
    }
}

static void paste(void)
{
    if (s_clip_len && insert(line_start(s_cur), s_clip, s_clip_len)) {
        s_cur = line_start(s_cur) + s_clip_len;
    }
}

static void find(bool ask)
{
    if (ask || !s_find[0]) {
        if (!app_input("Find", "Text to find (not case-sensitive):", s_find, sizeof(s_find)) || !s_find[0]) {
            return;
        }
    }
    int n = text_len(), k = (int)strlen(s_find);
    for (int step = 1; step <= n; step++) {
        int i = (s_cur + step) % n;
        int j = 0;
        while (j < k && i + j < n) {
            char a = at(i + j), b = s_find[j];
            if (a >= 'A' && a <= 'Z') {
                a += 32;
            }
            if (b >= 'A' && b <= 'Z') {
                b += 32;
            }
            if (a != b) {
                break;
            }
            j++;
        }
        if (j == k) {
            s_cur = i;
            scroll_to_cursor(true);
            return;
        }
    }
    app_toast("Not found.");
    sleep_ms(700);
}

static void go_to_line(void)
{
    char buf[12] = "";
    if (!app_input("Go to line", "Line number (0 for the end):", buf, sizeof(buf)) || !buf[0]) {
        return;
    }
    long want = strtol(buf, NULL, 10);
    int pos = 0, line = 1;
    if (want <= 0) {
        pos = text_len();
    } else {
        while (line < want) {
            int next = next_line(pos);
            if (next < 0) {
                break;
            }
            pos = next;
            line++;
        }
    }
    s_cur = pos;
    scroll_to_cursor(true);
}

// ---- screen -------------------------------------------------------------------------------------
static void draw(void)
{
    char title[64];
    snprintf(title, sizeof(title), "%s%s", s_path[0] ? app_basename(s_path) : "untitled", s_modified ? " *" : "");
    app_header(title, NULL);
    app_clear_body();
    lcd_fill_rect(0, APP_TOP, TEXT_X - 3, APP_BODY_H - STATUS_H, C_PANEL);

    int cur_ls = line_start(s_cur), cur_line = 0, top_line = line_number(s_top);
    int ls = s_top;
    for (int row = 0; row < ROWS && ls >= 0; row++) {
        int y = APP_TOP + 2 + row * ROW_H;
        bool here = ls == cur_ls;
        if (here) {
            lcd_fill_rect(TEXT_X - 3, y - 1, LCD_W - TEXT_X + 3, ROW_H, C_PANEL);
            cur_line = top_line + row;
        }
        char num[8];
        snprintf(num, sizeof(num), "%4d", (top_line + row) % 10000);
        lcd_text(2, y, num, here ? C_ACCENT : C_LINE, FONT_SMALL);

        char vis[COLS + 1];
        int le = line_end(ls), c = 0, i = ls;
        memset(vis, ' ', COLS);
        vis[COLS] = '\0';
        for (; i < le && c < s_left + COLS; i++) {
            char ch = at(i);
            int nc = ch == '\t' ? (c / TAB + 1) * TAB : c + 1;
            if (ch != '\t') {
                if (c >= s_left) {
                    vis[c - s_left] = ((uint8_t)ch >= 0x20 && (uint8_t)ch < 0x7F) ? ch : '?';
                }
            }
            c = nc;
        }
        lcd_text(TEXT_X, y, vis, C_FG, FONT_SMALL);
        if (i < le) {
            lcd_fill_rect(LCD_W - 2, y, 2, ROW_H - 2, C_ACCENT); // the line goes on
        }
        if (s_left > 0 && le > ls) {
            lcd_fill_rect(TEXT_X - 3, y, 1, ROW_H - 2, C_ACCENT);
        }
        if (here) {
            int cc = col_of(ls, s_cur) - s_left;
            if (cc >= 0 && cc < COLS) {
                int cx = TEXT_X + cc * 6;
                lcd_fill_rect(cx, y - 1, 6, ROW_H, C_ACCENT);
                char ch = s_cur < le ? at(s_cur) : ' ';
                char one[2] = {((uint8_t)ch >= 0x20 && (uint8_t)ch < 0x7F) ? ch : ' ', 0};
                lcd_text(cx, y, one, C_BG, FONT_SMALL);
            }
        }
        ls = next_line(ls);
    }

    // the status line
    int sy = APP_BOTTOM - STATUS_H;
    lcd_fill_rect(0, sy, LCD_W, STATUS_H, C_FIELD);
    char st[80];
    int total = text_len();
    snprintf(st, sizeof(st), "Ln %d/%d  Col %d  %d.%d KB  %s", cur_line ? cur_line : line_number(s_cur), line_count(),
             col_of(cur_ls, s_cur) + 1, total / 1024, total % 1024 * 10 / 1024, s_crlf ? "CRLF" : "LF");
    lcd_text(4, sy + 2, st, C_FG, FONT_SMALL);
    if (s_clip_len) {
        lcd_text(LCD_W - 4 - 4 * 6, sy + 2, "CLIP", C_ACC2, FONT_SMALL);
    }
    app_footer("F1 Help  F2 Save  F3 Open  F4 Find  Esc Quit");
    lcd_flush();
}

static const char *const HELP[] = {
    "# Moving",
    "Arrows     Move; Home / End: start / end of line",
    "PgUp/PgDn  A screen up / down",
    "Ctrl+G     Go to a line (0: the end)",
    "",
    "# Editing",
    "Enter      New line, keeping the indentation",
    "Tab        Spaces to the next 4-column stop",
    "Bksp/Del   Delete before / at the cursor",
    "Ctrl+X     Cut the line (again: add the next)",
    "Ctrl+C     Copy the line (again: add the next)",
    "Ctrl+V     Paste the lines above the cursor",
    "",
    "# Files",
    "F2 Ctrl+S  Save",
    "F7         Save as (Shift+F2)",
    "F3 Ctrl+O  Open a file",
    "Ctrl+N     New file",
    "F4 Ctrl+F  Find; F5 finds the next one",
    "Esc        Quit (asks to save changes)",
    "",
    "Files keep their line ends (LF or CRLF). The",
    "editor holds files up to " TEXT_KB ".",
};

int main(void)
{
    app_init();
    char last[sizeof(s_path)];
    FILINFO fi;
    if (app_setting_get("last", last, sizeof(last)) && f_stat(last, &fi) == FR_OK) {
        load(last);
    }
    for (;;) {
        scroll_to_cursor(false);
        draw();
        int k = app_wait_key();
        int len = text_len(), ls = line_start(s_cur);
        bool vertical = false;
        switch (k) {
        case KEY_LEFT:
            if (s_cur > 0) {
                s_cur--;
            }
            break;
        case KEY_RIGHT:
            if (s_cur < len) {
                s_cur++;
            }
            break;
        case KEY_UP:
        case KEY_DOWN:
        case KEY_PAGE_UP:
        case KEY_PAGE_DOWN: {
            if (s_want_col < 0) {
                s_want_col = col_of(ls, s_cur);
            }
            int steps = (k == KEY_UP || k == KEY_DOWN) ? 1 : ROWS - 1;
            bool down = k == KEY_DOWN || k == KEY_PAGE_DOWN;
            for (int i = 0; i < steps; i++) {
                int to = down ? next_line(ls) : prev_line(ls);
                if (to < 0) {
                    break;
                }
                ls = to;
                if (steps > 1) {
                    int t = down ? next_line(s_top) : prev_line(s_top);
                    if (t >= 0) {
                        s_top = t;
                    }
                }
            }
            s_cur = pos_at_col(ls, s_want_col);
            vertical = true;
            break;
        }
        case KEY_HOME: {
            // to the first non-blank, then to the start of the line
            int first = ls;
            while (first < len && (at(first) == ' ' || at(first) == '\t')) {
                first++;
            }
            s_cur = s_cur == first ? ls : first;
            break;
        }
        case KEY_END:
            s_cur = line_end(s_cur);
            break;
        case KEY_ENTER:
            type_newline();
            break;
        case KEY_TAB:
            type_tab();
            break;
        case KEY_BACKSPACE:
            if (s_cur > 0) {
                erase(s_cur - 1, 1);
                s_cur--;
            }
            break;
        case KEY_DEL:
            if (s_cur < len) {
                erase(s_cur, 1);
            }
            break;
        case KEY_CTRL_X:
            clip_line(true, s_last_key == KEY_CTRL_X);
            break;
        case KEY_CTRL_C:
            clip_line(false, s_last_key == KEY_CTRL_C);
            break;
        case KEY_CTRL_V:
            paste();
            break;
        case KEY_F2:
        case KEY_CTRL('s'):
            if (save()) {
                draw();
                app_toast("Saved.");
                sleep_ms(500);
            }
            break;
        case KEY_F7:
            save_as();
            break;
        case KEY_F3:
        case KEY_CTRL('o'):
            open_file();
            break;
        case KEY_CTRL('n'):
            if (may_discard()) {
                clear_text();
                s_path[0] = '\0';
            }
            break;
        case KEY_F4:
        case KEY_CTRL('f'):
            find(true);
            break;
        case KEY_F5:
            find(false);
            break;
        case KEY_CTRL('g'):
            go_to_line();
            break;
        case KEY_F1:
            app_help("Text Editor help", HELP, sizeof(HELP) / sizeof(HELP[0]));
            break;
        case KEY_ESC:
            if (may_discard()) {
                app_exit(NULL);
            }
            break;
        default:
            if (k >= 0x20 && k < 0x7F) {
                char ch = (char)k;
                if (insert(s_cur, &ch, 1)) {
                    s_cur++;
                }
            }
            break;
        }
        if (!vertical) {
            s_want_col = -1;
        }
        s_last_key = k;
    }
}
