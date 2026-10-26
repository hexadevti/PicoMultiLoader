// shell.c - the terminal's commands that only need the card and the hardware (see shell.h).
#include "shell.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "plat.h"
#include "apps.h"
#include "clock.h"
#include "config.h"
#include "keyboard.h"
#include "look.h"
#include "sdcard.h"
#include "uf2_image.h"
#if PICOMULTILOADER_WIFI
#include "webserver.h"
#endif

#define PATH_CAP  256
#define LINE_CAP  128
#define CAT_LINES 1000

typedef struct {
    const char *name, *args, *desc;
} cmd_t;

static const cmd_t CMDS[] = {
    {"help", "[command]", "list the commands, or explain one"},
    {"apps", "", "list the apps on Home"},
    {"run", "<app or file" PML_APP_EXT ">", "run an app, or flash and run a file"},
    {"ls", "[folder]", "list a folder"},
    {"cd", "[folder]", "change folder (no folder: the root)"},
    {"pwd", "", "show the folder you are in"},
    {"cat", "<file>", "show a text file"},
    {"view", "<file>", "read a text file in pages, or show a .bmp"},
    {"shot", "", "save a screenshot in /pml/shots (F10 too)"},
    {"files", "", "open this folder in Files"},
    {"home", "", "go to Home"},
    {"ctl", "", "open the control center"},
    {"theme", "[name | next]", "show or change the colour theme"},
    {"date", "", "show the date and time (set from the web)"},
    {"tz", "[auto | +hh:mm]", "show or set the time zone"},
    {"recent", "", "the apps run lately"},
    {"fav", "", "the favourites"},
#if PICOMULTILOADER_WIFI
    {"wifi", "[on | off]", "Wi-Fi and the web server"},
    {"ip", "", "the web server's address"},
#endif
    {"usb", "", "USB drive mode"},
    {"battery", "", "the battery charge"},
    {"free", "", "free memory"},
    {"df", "", "free space on the SD card"},
    {"about", "", "the loader, the chip and the installed app"},
    {"uname", "", "the system's name"},
    {"echo", "<text>", "print the text"},
    {"history", "", "the last commands"},
    {"clear", "", "clear the screen"},
    {"exit", "", "close the terminal (F5 does too)"},
    {"reboot", "", "restart: the installed app starts"},
};
#define NCMDS (int)(sizeof(CMDS) / sizeof(CMDS[0]))

static char s_cwd[PATH_CAP]; // "" = the root

const char *shell_cwd(void)
{
    return s_cwd;
}

int shell_split(char *line, char **argv, int max)
{
    int n = 0;
    char *p = line;
    while (n < max) {
        while (*p == ' ') {
            p++;
        }
        if (!*p) {
            break;
        }
        bool quoted = *p == '"';
        p += quoted;
        argv[n++] = p;
        while (*p && (quoted ? *p != '"' : *p != ' ')) {
            p++;
        }
        if (*p) {
            *p++ = '\0';
        }
    }
    return n;
}

bool shell_resolve(char *out, size_t len, const char *arg)
{
    char tmp[PATH_CAP];
    strlcpy(tmp, arg[0] == '/' ? "" : s_cwd, sizeof(tmp));
    for (const char *p = arg; *p;) {
        while (*p == '/') {
            p++;
        }
        size_t n = strcspn(p, "/");
        if (n == 0) {
            break;
        }
        if (n == 2 && p[0] == '.' && p[1] == '.') {
            char *slash = strrchr(tmp, '/');
            if (slash) {
                *slash = '\0';
            }
        } else if (!(n == 1 && p[0] == '.')) {
            size_t l = strlen(tmp);
            if (l + 1 + n >= sizeof(tmp)) {
                return false;
            }
            tmp[l] = '/';
            memcpy(tmp + l + 1, p, n);
            tmp[l + 1 + n] = '\0';
        }
        p += n;
    }
    strlcpy(out, tmp, len);
    return true;
}

static bool is_dir(const char *path)
{
    FILINFO fi;
    return !path[0] || (f_stat(path, &fi) == FR_OK && (fi.fattrib & AM_DIR));
}

static bool hidden(const FILINFO *fi)
{
    return fi->fname[0] == '.' || (fi->fattrib & (AM_HID | AM_SYS)) ||
           strcasecmp(fi->fname, "System Volume Information") == 0;
}

static void outf(shell_out_t out, bool error, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void outf(shell_out_t out, bool error, const char *fmt, ...)
{
    char line[LINE_CAP];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    out(line, error);
}

static void format_size(char *out, size_t len, uint64_t bytes)
{
    if (bytes < 10000) {
        snprintf(out, len, "%u B", (unsigned)bytes);
    } else if (bytes < 10000 * 1024) {
        snprintf(out, len, "%u KB", (unsigned)((bytes + 1023) / 1024));
    } else {
        snprintf(out, len, "%u MB", (unsigned)(bytes / (1024 * 1024)));
    }
}

// The path an argument names, or an error printed and false.
static bool arg_path(char *out, size_t len, const char *cmd, const char *arg, shell_out_t out_fn)
{
    if (!shell_resolve(out, len, arg)) {
        outf(out_fn, true, "%s: the path is too long", cmd);
        return false;
    }
    return true;
}

// ---- commands ---------------------------------------------------------------------------------
static void cmd_help(int argc, char **argv, shell_out_t out)
{
    for (int i = 0; i < NCMDS; i++) {
        if (argc < 2) {
            outf(out, false, "%-8s%s", CMDS[i].name, CMDS[i].desc);
        } else if (strcasecmp(argv[1], CMDS[i].name) == 0) {
            outf(out, false, "%s %s", CMDS[i].name, CMDS[i].args);
            outf(out, false, "  %s", CMDS[i].desc);
            return;
        }
    }
    if (argc < 2) {
        out("Tab completes, Up/Down recalls, Esc clears the line.", false);
    } else {
        outf(out, true, "help: no command %s", argv[1]);
    }
}

static void cmd_ls(int argc, char **argv, shell_out_t out)
{
    char path[PATH_CAP];
    if (!arg_path(path, sizeof(path), "ls", argc > 1 ? argv[1] : "", out)) {
        return;
    }
    FF_DIR dir;
    if (f_opendir(&dir, sd_path(path)) != FR_OK) {
        outf(out, true, "ls: no folder %s", argc > 1 ? argv[1] : "/");
        return;
    }
    FILINFO fi;
    int dirs = 0, files = 0;
    while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
        if (hidden(&fi)) {
            continue;
        }
        if (fi.fattrib & AM_DIR) {
            dirs++;
            outf(out, false, "%s/", fi.fname);
        } else {
            char size[16];
            format_size(size, sizeof(size), fi.fsize);
            files++;
            outf(out, false, "%-40.40s %10s", fi.fname, size);
        }
    }
    f_closedir(&dir);
    outf(out, false, "%d folder%s, %d file%s", dirs, dirs == 1 ? "" : "s", files, files == 1 ? "" : "s");
}

static void cmd_cd(int argc, char **argv, shell_out_t out)
{
    char path[PATH_CAP];
    if (!arg_path(path, sizeof(path), "cd", argc > 1 ? argv[1] : "/", out)) {
        return;
    }
    if (!is_dir(path)) {
        outf(out, true, "cd: no folder %s", argv[1]);
        return;
    }
    strlcpy(s_cwd, path, sizeof(s_cwd));
}

static void cmd_cat(int argc, char **argv, shell_out_t out, const char *file)
{
    char path[PATH_CAP];
    if (!file) {
        if (argc < 2) {
            out("cat: which file?", true);
            return;
        }
        if (!arg_path(path, sizeof(path), "cat", argv[1], out)) {
            return;
        }
        file = path;
    }
    FIL f;
    if (f_open(&f, file, FA_READ) != FR_OK) {
        if (argc < 2) {
            out("(none)", false); // recent / fav with no list yet
        } else {
            outf(out, true, "cat: no file %s", argv[1]);
        }
        return;
    }
    char line[LINE_CAP];
    for (int n = 0; f_gets(line, sizeof(line), &f); n++) {
        if (n == CAT_LINES) {
            out("cat: (stopped after 1000 lines)", true);
            break;
        }
        line[strcspn(line, "\r\n")] = '\0';
        for (char *c = line; *c; c++) {
            if (*c == '\t') {
                *c = ' ';
            } else if ((unsigned char)*c < 0x20) {
                *c = '.';
            }
        }
        out(line, false);
    }
    f_close(&f);
}

static void cmd_theme(int argc, char **argv, shell_out_t out)
{
    if (argc > 1) {
        char next[24], was[24];
        strlcpy(was, theme_name(), sizeof(was));
        if (strcasecmp(argv[1], "next") == 0) {
            theme_next(next, sizeof(next));
        } else {
            strlcpy(next, argv[1], sizeof(next));
        }
        if (!theme_load(next)) { // it fell back to the first built-in one
            outf(out, true, "theme: no theme %s", next);
            theme_load(was);
        } else {
            config_set("theme", theme_name());
        }
    }
    outf(out, false, "Theme: %s", theme_name());
}

static void cmd_date(shell_out_t out)
{
    clock_tm_t tm;
    char date[24];
    if (!clock_local(&tm)) {
        outf(out, true, "date: not known yet.%s", PICOMULTILOADER_WIFI ? " Turn Wi-Fi on (F4) to set it." : "");
        return;
    }
    clock_date_text(&tm, date, sizeof(date));
    outf(out, false, "%s %02d:%02d:%02d %s", date, tm.hour, tm.min, tm.sec, clock_zone_name());
}

static void cmd_tz(int argc, char **argv, shell_out_t out)
{
    if (argc > 1 && !clock_set_setting(argv[1])) {
        out("tz: use auto, or an offset like +01:00 or -03:00", true);
        return;
    }
    char v[16];
    bool manual = config_get("tz", v, sizeof(v)) && v[0];
    int off = clock_offset(), a = off < 0 ? -off : off;
    outf(out, false, "Time zone: %s (UTC%c%02d:%02d)%s", clock_zone_name(), off < 0 ? '-' : '+', a / 3600,
         a / 60 % 60, manual ? ", set by hand" : ", from the web");
}

static void cmd_battery(shell_out_t out)
{
    bool charging = false;
    int pct = kbd_battery(&charging);
    if (pct < 0) {
        out("battery: the keyboard does not answer", true);
    } else {
        outf(out, false, "Battery %d%%%s", pct, charging ? ", charging" : "");
    }
}

static void cmd_free(shell_out_t out)
{
    size_t total, free_bytes;
    plat_heap(&free_bytes, &total);
    outf(out, false, "Memory: %u KB free of %u KB (heap)", (unsigned)(free_bytes / 1024), (unsigned)(total / 1024));
}

static void cmd_df(shell_out_t out)
{
    uint64_t total, free_bytes;
    if (!sdcard_space(&total, &free_bytes)) {
        out("df: no SD card", true);
        return;
    }
    outf(out, false, "SD card: %u MB free of %u MB", (unsigned)(free_bytes >> 20), (unsigned)(total >> 20));
}

static void cmd_about(shell_out_t out)
{
    outf(out, false, "PicoMultiLoader for the PicoCalc, %s", uf2_chip_name());
    outf(out, false, "Built %s", __DATE__);
    char name[40], version[24];
    if (uf2_installed(name, sizeof(name), version, sizeof(version))) {
        outf(out, false, "Installed app: %s %s", name, version);
    } else {
        out("Installed app: (none)", false);
    }
    outf(out, false, "App area: %u KB", (unsigned)(uf2_flash_capacity() / 1024));
#if PICOMULTILOADER_WIFI
    out("Wi-Fi: yes", false);
#else
    out("Wi-Fi: no (not a Pico W)", false);
#endif
}

#if PICOMULTILOADER_WIFI
static void cmd_ip(shell_out_t out)
{
    web_status_t st;
    webserver_status(&st);
    switch (st.state) {
    case WEB_OFF:
        out("Wi-Fi is off. wifi on starts it.", false);
        break;
    case WEB_STARTING:
        out("Wi-Fi is starting...", false);
        break;
    case WEB_CONNECTING:
        outf(out, false, "Joining %s...", st.ssid);
        break;
    case WEB_RUNNING:
        outf(out, false, "%s %s", st.ap_mode ? "Own network" : "Network", st.ssid);
        if (st.ap_mode) {
            outf(out, false, "Password %s", st.password);
        }
        if (st.ip[0]) {
            outf(out, false, "http://%s/", st.ip);
        } else {
            out("(reconnecting...)", true);
        }
        break;
    default:
        outf(out, true, "Wi-Fi failed: %s", st.error);
        break;
    }
}
#endif

bool shell_run(int argc, char **argv, shell_out_t out)
{
    const char *cmd = argv[0];
    if (strcasecmp(cmd, "help") == 0) {
        cmd_help(argc, argv, out);
    } else if (strcasecmp(cmd, "ls") == 0) {
        cmd_ls(argc, argv, out);
    } else if (strcasecmp(cmd, "cd") == 0) {
        cmd_cd(argc, argv, out);
    } else if (strcasecmp(cmd, "pwd") == 0) {
        out(sd_path(s_cwd), false);
    } else if (strcasecmp(cmd, "cat") == 0) {
        cmd_cat(argc, argv, out, NULL);
    } else if (strcasecmp(cmd, "recent") == 0) {
        cmd_cat(1, argv, out, PML_DIR "/recents.txt");
    } else if (strcasecmp(cmd, "fav") == 0) {
        cmd_cat(1, argv, out, PML_DIR "/favourites.txt");
    } else if (strcasecmp(cmd, "theme") == 0) {
        cmd_theme(argc, argv, out);
    } else if (strcasecmp(cmd, "date") == 0) {
        cmd_date(out);
    } else if (strcasecmp(cmd, "tz") == 0) {
        cmd_tz(argc, argv, out);
    } else if (strcasecmp(cmd, "battery") == 0) {
        cmd_battery(out);
    } else if (strcasecmp(cmd, "free") == 0) {
        cmd_free(out);
    } else if (strcasecmp(cmd, "df") == 0) {
        cmd_df(out);
    } else if (strcasecmp(cmd, "about") == 0) {
        cmd_about(out);
    } else if (strcasecmp(cmd, "uname") == 0) {
        outf(out, false, "PicoMultiLoader %s", uf2_chip_name());
#if PICOMULTILOADER_WIFI
    } else if (strcasecmp(cmd, "ip") == 0) {
        cmd_ip(out);
#endif
    } else if (strcasecmp(cmd, "echo") == 0) {
        char line[LINE_CAP] = "";
        for (int i = 1; i < argc; i++) {
            if (i > 1) {
                strlcat(line, " ", sizeof(line));
            }
            strlcat(line, argv[i], sizeof(line));
        }
        out(line, false);
    } else if (strcasecmp(cmd, "reboot") == 0) {
        out("Restarting...", false);
        sleep_ms(100); // let the console catch up
        plat_reboot();
    } else {
        return false;
    }
    return true;
}

// ---- Tab completion ---------------------------------------------------------------------------
// Collects the names that start with a prefix: how many, the first one, and how long their common
// start is. The choices are printed on lines as they come (only when there are several).
typedef struct {
    const char *prefix;
    size_t plen;
    int count;
    char first[64];
    bool first_dir;
    size_t common;
    char list[LINE_CAP];
} matches_t;

static void match(matches_t *m, const char *name, bool dir)
{
    if (strncasecmp(name, m->prefix, m->plen) != 0) {
        return;
    }
    if (m->count++ == 0) {
        strlcpy(m->first, name, sizeof(m->first));
        m->first_dir = dir;
        m->common = strlen(m->first);
    } else {
        size_t k = m->plen;
        while (k < m->common && name[k] && tolower((unsigned char)name[k]) == tolower((unsigned char)m->first[k])) {
            k++;
        }
        m->common = k;
    }
    char item[70];
    snprintf(item, sizeof(item), "%s%s  ", name, dir ? "/" : "");
    strlcat(m->list, item, sizeof(m->list));
}

static void flush_list(matches_t *m, shell_out_t out, bool last)
{
    if (m->count > 1 && (last || strlen(m->list) > 40)) {
        out(m->list, false);
        m->list[0] = '\0';
    }
}

void shell_complete(char *line, size_t cap, shell_out_t out)
{
    char *word = strrchr(line, ' ');
    word = word ? word + 1 : line;
    bool first_word = word == line;
    matches_t m = {0};
    if (first_word) {
        m.prefix = word;
        m.plen = strlen(word);
        for (int i = 0; i < NCMDS; i++) {
            match(&m, CMDS[i].name, false);
            flush_list(&m, out, false);
        }
    } else {
        char *slash = strrchr(word, '/');
        m.prefix = slash ? slash + 1 : word;
        m.plen = strlen(m.prefix);
        char dir_arg[PATH_CAP], dir_path[PATH_CAP];
        size_t dl = slash ? (size_t)(slash - word) + 1 : 0; // keeps the "/"
        snprintf(dir_arg, sizeof(dir_arg), "%.*s", (int)dl, word);
        FF_DIR dir;
        if (!shell_resolve(dir_path, sizeof(dir_path), dir_arg) || f_opendir(&dir, sd_path(dir_path)) != FR_OK) {
            return;
        }
        FILINFO fi;
        while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
            if (!hidden(&fi)) {
                match(&m, fi.fname, (fi.fattrib & AM_DIR) != 0);
                flush_list(&m, out, false);
            }
        }
        f_closedir(&dir);
    }
    flush_list(&m, out, true);
    if (!m.count) {
        return;
    }
    // Put the completed part in place (the case from the name), then a "/" or " " when it is unique
    size_t at = (size_t)(m.prefix - line);
    if (at + m.common + 2 > cap) {
        return;
    }
    memcpy(line + at, m.first, m.common);
    line[at + m.common] = '\0';
    if (m.count == 1) {
        strlcat(line, m.first_dir ? "/" : " ", cap);
    }
}
