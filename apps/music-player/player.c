// player.c - play WAV files from the SD card through the PicoCalc's speakers or headphones.
//
// The screen is a folder list with a "now playing" panel under it. Playing a file makes the
// playlist: the WAV files of its folder, in name order (or shuffled). The sound is fed from the
// idle hook (app_set_idle), so it keeps playing while a dialog or the help screen is open.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "pico/stdlib.h"
#include "audio.h"
#include "pmlapp.h"

#define MAX_ENTRIES 256
#define POOL_SIZE   (16 * 1024)

#define ROW_H   11
#define PANEL_H 56
#define LIST_Y  (APP_TOP + 2)
#define ROWS    ((APP_BODY_H - PANEL_H - 4) / ROW_H)

// ---- name lists (the folder on screen, and the playlist) ----------------------------------------
typedef struct {
    char pool[POOL_SIZE];
    uint16_t off[MAX_ENTRIES];
    uint8_t dir[MAX_ENTRIES];
    int count, used;
} names_t;

static names_t s_list; // the folder being browsed: folders first, then .wav files
static names_t s_play; // the playlist: the .wav files of the playing folder
static char s_dir[256], s_play_dir[256];

static const char *name_at(const names_t *n, int i)
{
    return &n->pool[n->off[i]];
}

static bool names_add(names_t *n, const char *s, bool dir)
{
    int len = (int)strlen(s) + 1;
    if (n->count >= MAX_ENTRIES || n->used + len > POOL_SIZE) {
        return false;
    }
    n->off[n->count] = (uint16_t)n->used;
    n->dir[n->count] = dir;
    memcpy(&n->pool[n->used], s, len);
    n->used += len;
    n->count++;
    return true;
}

// Folders first, then by name: an insertion sort (the lists are short).
static void names_sort(names_t *n)
{
    for (int i = 1; i < n->count; i++) {
        uint16_t o = n->off[i];
        uint8_t d = n->dir[i];
        int j = i - 1;
        while (j >= 0 && (n->dir[j] < d || (n->dir[j] == d && strcasecmp(&n->pool[n->off[j]], &n->pool[o]) > 0))) {
            n->off[j + 1] = n->off[j];
            n->dir[j + 1] = n->dir[j];
            j--;
        }
        n->off[j + 1] = o;
        n->dir[j + 1] = d;
    }
}

// Read `path` into `n`: with folders if `with_dirs`, and the .wav files.
static bool names_read(names_t *n, const char *path, bool with_dirs)
{
    n->count = n->used = 0;
    DIR d;
    FILINFO fi;
    if (f_opendir(&d, path[0] ? path : "/") != FR_OK) {
        return false;
    }
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        if (fi.fattrib & (AM_HID | AM_SYS) || fi.fname[0] == '.') {
            continue;
        }
        bool dir = fi.fattrib & AM_DIR;
        if ((dir && with_dirs) || (!dir && app_has_ext(fi.fname, ".wav"))) {
            if (!names_add(n, fi.fname, dir)) {
                break;
            }
        }
    }
    f_closedir(&d);
    names_sort(n);
    return true;
}

// ---- playing ------------------------------------------------------------------------------------
enum { REPEAT_OFF, REPEAT_ALL, REPEAT_ONE };

static FIL s_file;
static bool s_open, s_paused, s_ended, s_shuffle;
static int s_repeat = REPEAT_ALL;
static int s_track = -1; // in s_play
static uint32_t s_rate, s_data_off, s_data_len, s_pos; // s_pos: bytes of data read
static int s_channels, s_bits, s_align;
static char s_error[64];
static uint8_t s_raw[AUDIO_FRAMES * 4];

static uint32_t rd16(const uint8_t *p)
{
    return p[0] | p[1] << 8;
}

static uint32_t rd32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static void close_track(void)
{
    if (s_open) {
        f_close(&s_file);
        s_open = false;
    }
}

// Open a WAV file and find its "fmt " and "data" chunks. Sets s_error on failure.
static bool open_wav(const char *path)
{
    close_track();
    s_error[0] = '\0';
    if (f_open(&s_file, path, FA_READ) != FR_OK) {
        strlcpy(s_error, "Can't open the file.", sizeof(s_error));
        return false;
    }
    s_open = true;
    uint8_t h[12];
    UINT n;
    if (f_read(&s_file, h, 12, &n) != FR_OK || n != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
        strlcpy(s_error, "Not a WAV file.", sizeof(s_error));
        close_track();
        return false;
    }
    bool fmt = false;
    for (;;) {
        uint8_t ch[8];
        if (f_read(&s_file, ch, 8, &n) != FR_OK || n != 8) {
            strlcpy(s_error, fmt ? "No sound data in the file." : "Bad WAV file.", sizeof(s_error));
            close_track();
            return false;
        }
        uint32_t size = rd32(ch + 4);
        FSIZE_t next = f_tell(&s_file) + size + (size & 1);
        if (!memcmp(ch, "fmt ", 4)) {
            uint8_t f[40] = {0};
            if (f_read(&s_file, f, size < sizeof(f) ? size : sizeof(f), &n) != FR_OK || n < 16) {
                break;
            }
            int tag = (int)rd16(f);
            if (tag == 0xFFFE && size >= 26) {
                tag = (int)rd16(f + 24); // WAVE_FORMAT_EXTENSIBLE: the sub-format
            }
            s_channels = (int)rd16(f + 2);
            s_rate = rd32(f + 4);
            s_align = (int)rd16(f + 12);
            s_bits = (int)rd16(f + 14);
            if (tag != 1 || (s_bits != 8 && s_bits != 16) || s_channels < 1 || s_channels > 2 ||
                s_align != s_channels * s_bits / 8) {
                snprintf(s_error, sizeof(s_error), "Only PCM WAV files play (8 or 16 bits, mono or stereo).");
                close_track();
                return false;
            }
            if (s_rate < 8000 || s_rate > 48000) {
                snprintf(s_error, sizeof(s_error), "Unsupported rate: %lu Hz (8000 to 48000).", (unsigned long)s_rate);
                close_track();
                return false;
            }
            fmt = true;
        } else if (!memcmp(ch, "data", 4)) {
            if (!fmt) {
                break;
            }
            s_data_off = (uint32_t)f_tell(&s_file);
            FSIZE_t left = f_size(&s_file) - s_data_off;
            s_data_len = size > left ? (uint32_t)left : size; // a cut-off file plays what it has
            s_data_len -= s_data_len % s_align;
            s_pos = 0;
            return true;
        }
        if (f_lseek(&s_file, next) != FR_OK) {
            break;
        }
    }
    strlcpy(s_error, "Bad WAV file.", sizeof(s_error));
    close_track();
    return false;
}

// The idle hook: keep the output's buffers full.
static void feed(void)
{
    if (!s_open || s_ended) {
        return;
    }
    uint32_t *buf;
    while ((buf = audio_buffer()) != NULL) {
        uint32_t want = AUDIO_FRAMES * (uint32_t)s_align, left = s_data_len - s_pos;
        if (want > left) {
            want = left;
        }
        UINT n = 0;
        if (want == 0 || f_read(&s_file, s_raw, want, &n) != FR_OK || n == 0) {
            s_ended = true;
            return;
        }
        s_pos += n;
        int frames = (int)n / s_align;
        const uint8_t *p = s_raw;
        for (int i = 0; i < frames; i++) {
            int16_t l, r;
            if (s_bits == 16) {
                l = (int16_t)rd16(p);
                r = s_channels == 2 ? (int16_t)rd16(p + 2) : l;
            } else {
                l = (int16_t)((p[0] - 128) << 8);
                r = s_channels == 2 ? (int16_t)((p[1] - 128) << 8) : l;
            }
            p += s_align;
            buf[i] = audio_frame(l, r);
        }
        audio_queue(frames);
    }
}

// Play a track of the playlist; `cut` drops what is still queued of the one before.
static bool play(int track, bool cut)
{
    if (track < 0 || track >= s_play.count) {
        return false;
    }
    char path[sizeof(s_play_dir) + 260];
    snprintf(path, sizeof(path), "%s/%s", s_play_dir, name_at(&s_play, track));
    s_track = track;
    s_ended = false;
    uint32_t old_rate = s_rate;
    bool was_open = s_open;
    if (!open_wav(path)) {
        audio_stop();
        return false;
    }
    if (!was_open || s_rate != old_rate) {
        audio_start(s_rate);
    } else if (cut) {
        audio_flush();
    }
    s_paused = false;
    audio_pause(false);
    feed();
    return true;
}

static void stop(void)
{
    audio_stop();
    close_track();
    s_track = -1;
    s_paused = false;
}

static void seek(int seconds)
{
    if (!s_open) {
        return;
    }
    int64_t p = (int64_t)s_pos + (int64_t)seconds * s_rate * s_align;
    if (p < 0) {
        p = 0;
    }
    if (p >= s_data_len) {
        p = s_data_len;
    }
    p -= p % s_align;
    audio_flush();
    s_pos = (uint32_t)p;
    f_lseek(&s_file, s_data_off + s_pos);
    s_ended = false;
    feed();
}

// The track after (dir 1) or before (dir -1) this one, or -1 at an end with repeat off.
static int next_track(int dir, bool user)
{
    int n = s_play.count;
    if (n == 0) {
        return -1;
    }
    if (s_repeat == REPEAT_ONE && !user) {
        return s_track;
    }
    if (s_shuffle && n > 1) {
        int t;
        do {
            t = rand() % n;
        } while (t == s_track);
        return t;
    }
    int t = s_track + dir;
    if (t < 0 || t >= n) {
        if (s_repeat == REPEAT_OFF && !user) {
            return -1;
        }
        t = (t + n) % n;
    }
    return t;
}

// Play the next playable track, skipping files that won't open (at most one round).
static void advance(int dir, bool user)
{
    for (int tries = 0; tries < s_play.count; tries++) {
        int t = next_track(dir, user);
        if (t < 0) {
            stop();
            return;
        }
        if (play(t, user)) {
            return;
        }
        s_track = t;
        user = true; // keep going past the bad one
    }
    stop();
}

// ---- the screen ---------------------------------------------------------------------------------
static int s_sel, s_scroll;

static void fmt_time(char *out, size_t len, uint32_t seconds)
{
    snprintf(out, len, "%lu:%02lu", (unsigned long)(seconds / 60), (unsigned long)(seconds % 60));
}

static void draw(void)
{
    app_header("Music Player", s_dir[0] ? s_dir : "/");
    app_clear_body();

    // the folder
    if (s_sel < s_scroll) {
        s_scroll = s_sel;
    } else if (s_sel >= s_scroll + ROWS) {
        s_scroll = s_sel - ROWS + 1;
    }
    bool here = strcmp(s_dir, s_play_dir) == 0 && s_track >= 0;
    const char *playing = s_track >= 0 ? name_at(&s_play, s_track) : "";
    for (int r = 0; r < ROWS; r++) {
        int i = s_scroll + r;
        if (i >= s_list.count) {
            break;
        }
        int y = LIST_Y + r * ROW_H;
        bool sel = i == s_sel;
        if (sel) {
            lcd_fill_rect(0, y - 1, LCD_W, ROW_H, C_SEL_BG);
        }
        const char *nm = name_at(&s_list, i);
        bool now = here && !s_list.dir[i] && strcmp(nm, playing) == 0;
        lcd_color_t c = sel ? C_SEL_FG : s_list.dir[i] ? C_ACC2 : now ? C_OK : C_FG;
        lcd_text(4, y, s_list.dir[i] ? "/" : now ? ">" : " ", c, FONT_SMALL);
        app_text_clip(14, y, nm, (LCD_W - 18) / 6, c, FONT_SMALL);
    }
    if (s_list.count == 0) {
        lcd_text(14, LIST_Y, "No folders or WAV files here.", C_DIM, FONT_SMALL);
    }
    if (s_list.count > ROWS) {
        // a scroll bar
        int bh = ROWS * ROW_H, th = bh * ROWS / s_list.count;
        th = th < 6 ? 6 : th;
        int ty = (bh - th) * s_scroll / (s_list.count - ROWS);
        lcd_fill_rect(LCD_W - 3, LIST_Y + ty, 2, th, C_LINE);
    }

    // the now playing panel
    int py = APP_BOTTOM - PANEL_H;
    lcd_fill_rect(0, py, LCD_W, PANEL_H, C_PANEL);
    lcd_hline(0, py, LCD_W, C_LINE);
    if (s_error[0]) {
        app_text_clip(6, py + 5, s_error, (LCD_W - 12) / 6, C_ERR, FONT_SMALL);
    } else if (s_track >= 0) {
        app_text_clip(6, py + 4, playing, (LCD_W - 12) / 7, C_BRIGHT, FONT_LARGE);
    } else {
        lcd_text(6, py + 4, "Stopped", C_DIM, FONT_LARGE);
    }
    uint32_t bps = s_rate * (uint32_t)(s_align ? s_align : 1);
    uint32_t pos = s_open && bps ? s_pos / bps : 0, len = s_open && bps ? s_data_len / bps : 0;
    int bx = 6, bw = LCD_W - 12, by = py + 22;
    lcd_fill_rect(bx, by, bw, 6, C_FIELD);
    if (len) {
        lcd_fill_rect(bx, by, (int)((int64_t)bw * s_pos / s_data_len), 6, C_ACCENT);
    }
    char t1[16], t2[16], line[96];
    fmt_time(t1, sizeof(t1), pos);
    fmt_time(t2, sizeof(t2), len);
    snprintf(line, sizeof(line), "%s / %s", t1, t2);
    lcd_text(6, py + 32, line, C_FG, FONT_SMALL);
    if (s_open) {
        snprintf(line, sizeof(line), "%lu Hz %d-bit %s", (unsigned long)s_rate, s_bits, s_channels == 2 ? "stereo" : "mono");
        lcd_text(LCD_W - 6 - (int)strlen(line) * 6, py + 32, line, C_DIM, FONT_SMALL);
    }
    const char *state = s_track < 0 ? "STOP" : s_paused ? "PAUSE" : "PLAY";
    static const char *const REPEAT[] = {"repeat off", "repeat all", "repeat one"};
    snprintf(line, sizeof(line), "%-5s  %s  %s  track %d/%d", state, REPEAT[s_repeat], s_shuffle ? "shuffle" : "in order",
             s_track >= 0 ? s_track + 1 : 0, s_play.count);
    lcd_text(6, py + 43, line, s_paused ? C_WARN : C_DIM, FONT_SMALL);
    // the volume
    int vx = LCD_W - 6 - AUDIO_VOLUME_MAX * 3;
    for (int i = 0; i < AUDIO_VOLUME_MAX; i++) {
        int h = 2 + i / 2;
        lcd_fill_rect(vx + i * 3, py + 51 - h, 2, h, i < audio_volume() ? C_ACCENT : C_LINE);
    }
    app_footer("F1 Help  Enter Play  Space Pause  Esc Quit");
    lcd_flush();
}

static void open_dir(const char *path, const char *select)
{
    strlcpy(s_dir, path, sizeof(s_dir));
    names_read(&s_list, s_dir, true);
    s_sel = s_scroll = 0;
    for (int i = 0; select && i < s_list.count; i++) {
        if (strcmp(name_at(&s_list, i), select) == 0) {
            s_sel = i;
            break;
        }
    }
    app_setting_set("dir", s_dir[0] ? s_dir : "/");
}

static void parent_dir(void)
{
    if (!s_dir[0] || strcmp(s_dir, "/") == 0) {
        return;
    }
    char up[sizeof(s_dir)], was[sizeof(s_dir)];
    strlcpy(was, app_basename(s_dir), sizeof(was));
    app_dirname(s_dir, up, sizeof(up));
    open_dir(up, was);
}

static void enter(void)
{
    if (s_sel >= s_list.count) {
        return;
    }
    const char *nm = name_at(&s_list, s_sel);
    if (s_list.dir[s_sel]) {
        char path[sizeof(s_dir) + 8];
        int n = snprintf(path, sizeof(path), "%s/%s", strcmp(s_dir, "/") == 0 ? "" : s_dir, nm);
        if (n >= (int)sizeof(s_dir)) {
            strlcpy(s_error, "The folder's path is too long.", sizeof(s_error));
            return;
        }
        open_dir(path, NULL);
        return;
    }
    // the playlist: the .wav files of this folder
    if (strcmp(s_play_dir, s_dir) != 0 || s_play.count == 0) {
        strlcpy(s_play_dir, strcmp(s_dir, "/") == 0 ? "" : s_dir, sizeof(s_play_dir));
        names_read(&s_play, s_play_dir, false);
    }
    for (int i = 0; i < s_play.count; i++) {
        if (strcmp(name_at(&s_play, i), nm) == 0) {
            play(i, true);
            return;
        }
    }
}

static void save_settings(void)
{
    char v[8];
    snprintf(v, sizeof(v), "%d", audio_volume());
    app_setting_set("volume", v);
    snprintf(v, sizeof(v), "%d", s_repeat);
    app_setting_set("repeat", v);
    app_setting_set("shuffle", s_shuffle ? "1" : "0");
}

static const char *const HELP[] = {
    "# The list",
    "Up/Down    Choose; PgUp / PgDn a page",
    "Enter      Play the file, or open the folder",
    "Bksp       Up a folder",
    "",
    "# Playing",
    "Space      Pause / resume",
    "Left/Right Back / on 5 seconds",
    "[ / ]      Back / on 30 seconds",
    "n / p      Next / previous track",
    "x          Stop",
    "+ / -      Volume",
    "r          Repeat: off, all, one",
    "s          Shuffle on / off",
    "Esc        Quit",
    "",
    "Plays PCM WAV files: 8 or 16 bits, mono or",
    "stereo, 8 to 48 kHz. Playing a file makes",
    "the folder's WAV files the playlist. It",
    "starts in /Music if there is one.",
};

int main(void)
{
    app_init();
    srand((unsigned)time_us_32());
    char v[256];
    if (app_setting_get("volume", v, sizeof(v))) {
        audio_set_volume(atoi(v));
    }
    if (app_setting_get("repeat", v, sizeof(v))) {
        s_repeat = atoi(v) % 3;
    }
    if (app_setting_get("shuffle", v, sizeof(v))) {
        s_shuffle = v[0] == '1';
    }
    FILINFO fi;
    if (app_setting_get("dir", v, sizeof(v)) && (strcmp(v, "/") == 0 || f_stat(v, &fi) == FR_OK)) {
        open_dir(strcmp(v, "/") == 0 ? "" : v, NULL);
    } else if (f_stat("/Music", &fi) == FR_OK) {
        open_dir("/Music", NULL);
    } else {
        open_dir("", NULL);
    }
    app_set_idle(feed);

    uint32_t next_draw = 0;
    bool dirty = true;
    for (;;) {
        if (s_ended && s_open) {
            advance(1, false);
            dirty = true;
        }
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (dirty || (s_track >= 0 && !s_paused && now >= next_draw)) {
            draw();
            next_draw = now + 500;
            dirty = false;
        }
        int k = app_poll_key();
        if (k == KEY_NONE) {
            continue;
        }
        dirty = true;
        int page = ROWS - 1;
        switch (k) {
        case KEY_UP:
            s_sel = s_sel > 0 ? s_sel - 1 : s_list.count - 1;
            break;
        case KEY_DOWN:
            s_sel = s_sel < s_list.count - 1 ? s_sel + 1 : 0;
            break;
        case KEY_PAGE_UP:
            s_sel = s_sel > page ? s_sel - page : 0;
            break;
        case KEY_PAGE_DOWN:
            s_sel = s_sel + page < s_list.count ? s_sel + page : s_list.count - 1;
            break;
        case KEY_HOME:
            s_sel = 0;
            break;
        case KEY_END:
            s_sel = s_list.count - 1;
            break;
        case KEY_ENTER:
            s_error[0] = '\0';
            enter();
            break;
        case KEY_BACKSPACE:
            parent_dir();
            break;
        case ' ':
            if (s_track >= 0) {
                s_paused = !s_paused;
                audio_pause(s_paused);
            } else if (s_sel < s_list.count && !s_list.dir[s_sel]) {
                enter();
            }
            break;
        case KEY_LEFT:
            seek(-5);
            break;
        case KEY_RIGHT:
            seek(5);
            break;
        case '[':
            seek(-30);
            break;
        case ']':
            seek(30);
            break;
        case 'n':
            if (s_track >= 0) {
                advance(1, true);
            }
            break;
        case 'p':
            if (s_track >= 0) {
                // back to the start first, like most players, unless near it
                if (s_rate && s_pos / (s_rate * (uint32_t)s_align) >= 3) {
                    seek(-1000000);
                } else {
                    advance(-1, true);
                }
            }
            break;
        case 'x':
            stop();
            break;
        case '+':
        case '=':
            audio_set_volume(audio_volume() + 1);
            save_settings();
            break;
        case '-':
            audio_set_volume(audio_volume() - 1);
            save_settings();
            break;
        case 'r':
            s_repeat = (s_repeat + 1) % 3;
            save_settings();
            break;
        case 's':
            s_shuffle = !s_shuffle;
            save_settings();
            break;
        case KEY_F1:
            app_help("Music Player help", HELP, sizeof(HELP) / sizeof(HELP[0]));
            break;
        case KEY_ESC:
            stop();
            app_exit(NULL);
            break;
        }
        if (s_sel < 0) {
            s_sel = 0;
        }
    }
}
