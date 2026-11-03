// editor.c - the text editor (see editor.h)
//
// The text is a gap buffer (one array with a hole at the cursor, so typing is cheap), allocated
// when a file opens and grown as it fills, up to ED_MAX. Lines end in '\n' in memory; a file that
// used CRLF is saved with CRLF again, and a UTF-8 BOM is kept. Tabs show as 4-column stops and the
// Tab key inserts spaces. Ctrl+X / Ctrl+C / Ctrl+V cut, copy and paste whole lines: pressing Ctrl+X
// or Ctrl+C again adds the next line, so a block moves in a few presses.
#include "editor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "keyboard.h"
#include "lcd.h"
#include "look.h"
#include "sdcard.h"

#if PML_RP2040
#define ED_MAX   (32 * 1024)
#elif PML_P4
#define ED_MAX   (512 * 1024)
#else
#define ED_MAX   (96 * 1024) // the RP2350's heap is ~205 KB
#endif
#define ED_STEP  (8 * 1024) // room added on top of the file, and the least a buffer grows by
#define CLIP_MAX (4 * 1024)
#define TAB      4
#define GUTTER   5 // line number columns (4 digits and a space)

static char *s_buf;
static int s_cap;
static int s_gap0, s_gap1;  // the gap is s_buf[s_gap0..s_gap1)
static int s_cur;           // the cursor, as an offset in the text
static int s_top;           // offset of the first line on screen
static int s_left;          // first column on screen
static int s_want_col = -1; // the column Up / Down aim for
static int s_cols, s_rows;  // the text area, without the line numbers
static bool s_modified, s_crlf, s_bom;
static char s_path[256];
static char *s_clip; // CLIP_MAX bytes once something is copied
static int s_clip_len;
static int s_last_key;
static const char *s_msg;

// ---- the buffer -------------------------------------------------------------------------------
static inline int text_len(void)
{
    return s_cap - (s_gap1 - s_gap0);
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

// Make the gap at least `n` bytes: a bigger buffer, the text after the gap moved to its end.
static bool make_room(int n)
{
    int need = n - (s_gap1 - s_gap0);
    if (need <= 0) {
        return true;
    }
    int want = s_cap + need + ED_STEP;
    want = want < s_cap + s_cap / 2 ? s_cap + s_cap / 2 : want;
    want = want > ED_MAX ? ED_MAX : want;
    if (want - s_cap < need) {
        s_msg = "The file is full.";
        return false;
    }
    char *p = realloc(s_buf, want);
    if (!p) {
        s_msg = "Out of memory.";
        return false;
    }
    int tail = s_cap - s_gap1;
    memmove(&p[want - tail], &p[s_gap1], tail);
    s_buf = p;
    s_gap1 = want - tail;
    s_cap = want;
    return true;
}

static bool insert(int pos, const char *s, int n)
{
    if (!make_room(n)) {
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

// ---- lines ------------------------------------------------------------------------------------
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

// Keep the cursor on screen (scrolling by lines), with `centre` after a jump.
static void scroll_to_cursor(bool centre)
{
    int cl = line_start(s_cur);
    if (cl < s_top) {
        s_top = cl;
    } else {
        int ls = s_top, row = 0;
        while (ls != cl && ls >= 0 && row < s_rows) {
            ls = next_line(ls);
            row++;
        }
        if (row >= s_rows || ls != cl) {
            s_top = cl;
            for (int i = 0; i < (centre ? s_rows / 2 : s_rows - 1) && s_top > 0; i++) {
                s_top = prev_line(s_top);
            }
        }
    }
    if (centre && cl == s_top) {
        for (int i = 0; i < s_rows / 2 && s_top > 0; i++) {
            s_top = prev_line(s_top);
        }
    }
    int c = col_of(cl, s_cur);
    if (c < s_left) {
        s_left = c > 8 ? c - 8 : 0;
    } else if (c >= s_left + s_cols) {
        s_left = c - s_cols + 9;
    }
}

// ---- files ------------------------------------------------------------------------------------
static bool fail(char *err, size_t len, const char *msg)
{
    snprintf(err, len, "%s", msg);
    return false;
}

static void reset(void)
{
    s_gap0 = 0;
    s_gap1 = s_cap;
    s_cur = s_top = s_left = 0;
    s_want_col = -1;
    s_modified = s_crlf = s_bom = false;
    s_last_key = 0;
    s_msg = NULL;
}

bool ed_open(const char *path, int cols, int rows, char *err, size_t len)
{
    ed_close();
    s_cols = cols - GUTTER;
    s_rows = rows;
    FIL *f = NULL;
    FSIZE_t size = 0;
    if (path[0]) {
        if (!(f = malloc(sizeof(FIL)))) {
            return fail(err, len, "Out of memory");
        }
        if (f_open(f, path, FA_READ) != FR_OK) {
            free(f);
            return fail(err, len, "Can't open the file");
        }
        size = f_size(f);
        if (size > ED_MAX - 1024) {
            f_close(f);
            free(f);
            snprintf(err, len, "Too big to edit: up to %d KB", (ED_MAX - 1024) / 1024);
            return false;
        }
    }
    s_cap = (int)size + ED_STEP > ED_MAX ? ED_MAX : (int)size + ED_STEP;
    if (!(s_buf = malloc(s_cap))) {
        if (f) {
            f_close(f);
            free(f);
        }
        s_cap = 0;
        return fail(err, len, "Out of memory");
    }
    reset();
    s_path[0] = '\0';
    if (!f) {
        return true;
    }
    UINT n = 0;
    bool ok = f_read(f, s_buf, (UINT)size, &n) == FR_OK && n == size;
    f_close(f);
    free(f);
    if (!ok) {
        ed_close();
        return fail(err, len, "Read error");
    }
    // Drop a UTF-8 BOM and the '\r' of CRLF line ends (both come back on saving).
    int start = 0, w = 0;
    if (n >= 3 && (uint8_t)s_buf[0] == 0xEF && (uint8_t)s_buf[1] == 0xBB && (uint8_t)s_buf[2] == 0xBF) {
        s_bom = true;
        start = 3;
    }
    for (int r = start; r < (int)n; r++) {
        if (s_buf[r] == '\r' && r + 1 < (int)n && s_buf[r + 1] == '\n') {
            s_crlf = true;
            continue;
        }
        s_buf[w++] = s_buf[r];
    }
    s_gap0 = w;
    strlcpy(s_path, path, sizeof(s_path));
    return true;
}

void ed_close(void)
{
    free(s_buf);
    free(s_clip);
    s_buf = s_clip = NULL;
    s_cap = s_gap0 = s_gap1 = s_clip_len = 0;
}

static bool write_all(FIL *f)
{
    char out[256];
    int o = 0, n = text_len();
    UINT wn;
    if (s_bom && (f_write(f, "\xEF\xBB\xBF", 3, &wn) != FR_OK || wn != 3)) {
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
bool ed_save(const char *path, char *err, size_t len)
{
    char to[sizeof(s_path)], tmp[sizeof(s_path) + 4];
    strlcpy(to, path && path[0] ? path : s_path, sizeof(to));
    if (!to[0]) {
        return fail(err, len, "No file name");
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", to);
    FIL *f = malloc(sizeof(FIL));
    if (!f) {
        return fail(err, len, "Out of memory");
    }
    if (f_open(f, tmp, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        free(f);
        return fail(err, len, "Can't create the file: is the folder there?");
    }
    bool ok = write_all(f);
    ok = f_close(f) == FR_OK && ok;
    free(f);
    if (!ok) {
        f_unlink(tmp);
        return fail(err, len, "Write error: the card may be full");
    }
    f_unlink(to);
    if (f_rename(tmp, to) != FR_OK) {
        return fail(err, len, "Saved as .tmp: couldn't rename it");
    }
    strlcpy(s_path, to, sizeof(s_path));
    s_modified = false;
    return true;
}

const char *ed_path(void)
{
    return s_path;
}

bool ed_modified(void)
{
    return s_modified;
}

const char *ed_take_message(void)
{
    const char *m = s_msg;
    s_msg = NULL;
    return m;
}

// ---- editing ----------------------------------------------------------------------------------
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
    int n = TAB - col_of(line_start(s_cur), s_cur) % TAB;
    if (insert(s_cur, "        ", n)) {
        s_cur += n;
    }
}

// Ctrl+X / Ctrl+C: the line (with its '\n') to the clipboard, added to it if `append`.
static void clip_line(bool cut, bool append)
{
    if (!s_clip && !(s_clip = malloc(CLIP_MAX))) {
        s_msg = "Out of memory.";
        return;
    }
    int ls = line_start(s_cur), next = next_line(ls);
    int n = (next < 0 ? text_len() : next) - ls;
    if (!append) {
        s_clip_len = 0;
    }
    if (s_clip_len + n + 1 > CLIP_MAX) {
        s_msg = "The clipboard is full.";
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
    int ls = line_start(s_cur);
    if (s_clip_len && insert(ls, s_clip, s_clip_len)) {
        s_cur = ls + s_clip_len;
    }
}

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

bool ed_find(const char *what)
{
    int n = text_len(), k = (int)strlen(what);
    for (int step = 1; k && step <= n; step++) {
        int i = (s_cur + step) % n, j = 0;
        while (j < k && i + j < n && lower(at(i + j)) == lower(what[j])) {
            j++;
        }
        if (j == k) {
            s_cur = i;
            s_want_col = -1;
            scroll_to_cursor(true);
            return true;
        }
    }
    return false;
}

void ed_goto_line(long n)
{
    int pos = 0;
    if (n <= 0) {
        pos = text_len();
    } else {
        for (long line = 1; line < n; line++) {
            int next = next_line(pos);
            if (next < 0) {
                break;
            }
            pos = next;
        }
    }
    s_cur = pos;
    s_want_col = -1;
    scroll_to_cursor(true);
}

bool ed_key(int key)
{
    int len = text_len(), ls = line_start(s_cur);
    bool vertical = false;
    switch (key) {
    case KEY_LEFT:
        s_cur -= s_cur > 0;
        break;
    case KEY_RIGHT:
        s_cur += s_cur < len;
        break;
    case KEY_UP:
    case KEY_DOWN:
    case KEY_PAGE_UP:
    case KEY_PAGE_DOWN: {
        if (s_want_col < 0) {
            s_want_col = col_of(ls, s_cur);
        }
        int steps = (key == KEY_UP || key == KEY_DOWN) ? 1 : s_rows - 1;
        bool down = key == KEY_DOWN || key == KEY_PAGE_DOWN;
        for (int i = 0; i < steps; i++) {
            int to = down ? next_line(ls) : prev_line(ls);
            if (to < 0) {
                break;
            }
            ls = to;
            if (steps > 1) {
                int t = down ? next_line(s_top) : prev_line(s_top);
                s_top = t >= 0 ? t : s_top;
            }
        }
        s_cur = pos_at_col(ls, s_want_col);
        vertical = true;
        break;
    }
    case KEY_HOME: { // to the first non-blank, then to the start of the line
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
    default:
        if (key < 0x20 || key >= 0x7F) {
            return false;
        }
        char ch = (char)key;
        if (insert(s_cur, &ch, 1)) {
            s_cur++;
        }
        break;
    }
    if (!vertical) {
        s_want_col = -1;
    }
    s_last_key = key;
    scroll_to_cursor(false);
    return true;
}

// ---- screen -----------------------------------------------------------------------------------
void ed_draw(int x, int y, int line_h)
{
    int tx = x + GUTTER * 6;
    lcd_fill_rect(0, y - 2, tx - 3, s_rows * line_h + 2, C_PANEL);
    int cur_ls = line_start(s_cur), top_line = line_number(s_top), ls = s_top;
    for (int row = 0; row < s_rows && ls >= 0; row++, ls = next_line(ls)) {
        int ry = y + row * line_h;
        bool here = ls == cur_ls;
        if (here) {
            lcd_fill_rect(tx - 3, ry - 1, LCD_W - tx + 3, line_h, C_PANEL);
        }
        char num[8];
        snprintf(num, sizeof(num), "%4d", (top_line + row) % 10000);
        lcd_text(x, ry, num, here ? C_ACCENT : C_LINE, FONT_SMALL);

        char vis[96];
        int cols = s_cols < (int)sizeof(vis) - 1 ? s_cols : (int)sizeof(vis) - 1;
        int le = line_end(ls), c = 0, i = ls;
        memset(vis, ' ', cols);
        vis[cols] = '\0';
        for (; i < le && c < s_left + cols; i++) {
            char ch = at(i);
            if (ch != '\t' && c >= s_left) {
                vis[c - s_left] = ((uint8_t)ch >= 0x20 && (uint8_t)ch < 0x7F) ? ch : '?';
            }
            c = ch == '\t' ? (c / TAB + 1) * TAB : c + 1;
        }
        lcd_text(tx, ry, vis, C_FG, FONT_SMALL);
        if (i < le) {
            lcd_fill_rect(LCD_W - 2, ry, 2, line_h - 2, C_ACCENT); // the line goes on
        }
        if (s_left > 0 && le > ls) {
            lcd_fill_rect(tx - 3, ry, 1, line_h - 2, C_ACCENT);
        }
        if (here) {
            int cc = col_of(ls, s_cur) - s_left;
            if (cc >= 0 && cc < cols) {
                int cx = tx + cc * 6;
                lcd_fill_rect(cx, ry - 1, 6, line_h, C_ACCENT);
                char ch = s_cur < le ? at(s_cur) : ' ';
                char one[2] = {((uint8_t)ch >= 0x20 && (uint8_t)ch < 0x7F) ? ch : ' ', '\0'};
                lcd_text(cx, ry, one, C_BG, FONT_SMALL);
            }
        }
    }
}

void ed_status(char *out, size_t len)
{
    int n = text_len(), ls = line_start(s_cur);
    snprintf(out, len, "Ln %d/%d  Col %d  %d.%d KB  %s%s", line_number(s_cur), line_number(n), col_of(ls, s_cur) + 1,
             n / 1024, n % 1024 * 10 / 1024, s_crlf ? "CRLF" : "LF", s_clip_len ? "  Clip" : "");
}
