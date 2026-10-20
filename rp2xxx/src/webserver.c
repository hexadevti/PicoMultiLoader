// webserver.c - F4 mode on the Pico 2 W: Wi-Fi through the CYW43439 (lwIP, polled from the main
// loop) and a small REST API plus an embedded single-page file manager for the SD card. The same
// API as the ESP32-P4 build, so both use common/web/index.html:
//
//   GET  /                         the file manager page
//   GET  /api/list?path=/dir       {"path","entries":[{"n":name,"d":is_dir,"s":size,"t":mtime}]}
//   GET  /api/info                 {"total","free","ssid","ap"} (bytes)
//   GET  /api/file?path=/f[&dl=1]  download / view a file
//   POST /api/upload?path=/f[&mtime=unix]   raw request body becomes the file (replaces it)
//   POST /api/mkdir?path=/dir
//   POST /api/delete?path=/f       files, or folders with everything in them
//   POST /api/rename?from=/a&to=/b
//   POST /api/flash?path=/f.uf2    hand the file to the loader to flash and boot
//   POST /api/wifi                 body "ssid\npassword": the network to join (same as F1)
//
// Paths are relative to the SD card root, URL-encoded, and may not contain "." or ".." parts.
// Everything runs in the main loop's context (lwIP NO_SYS), so FatFS is never used concurrently.
#include "webserver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "pico/cyw43_arch.h"
#include "pico/rand.h"
#include "pico/stdlib.h"
#include "lwip/apps/mdns.h"
#include "lwip/tcp.h"
#include "config.h"
#include "dhcpserver.h"
#include "index_html.h"
#include "sdcard.h"

#define CONNECT_TIMEOUT_MS 20000
#define AP_SSID_PREFIX     "PicoMultiLoader"
#define HTTP_PORT          80
#define MAX_CONNS          3 // each takes ~5 KB of heap, and the heap is small next to the radio firmware
#define HDR_MAX            2048
#define OUT_MAX            1460 // one TCP segment; keeps two connections within the small heap
#define PATH_LEN           256
#define PARAM_LEN          (3 * PATH_LEN)
#define BODY_MAX           160
#define IDLE_POLLS         6 // x 5 s without progress drops a connection
#define POLL_INTERVAL      10 // lwIP coarse timer ticks (0.5 s)

static web_status_t s_status = { .state = WEB_OFF, .progress = -1 };
static uint32_t s_rev;
static uint32_t s_changes; // bumped whenever a browser changes the card
static char s_flash_path[PATH_LEN];
static bool s_flash_pending;

static bool s_radio_ready;
static uint64_t s_start_at_us;
static uint64_t s_join_deadline_us, s_join_retry_us;
static bool s_join_saw_nonet;
static char s_join_ssid[33];
static char s_join_pass[65];
static uint64_t s_link_check_us;
static struct tcp_pcb *s_listen;
static struct netif *s_netif; // the interface the server runs on
static bool s_mdns;

// ---- status -----------------------------------------------------------------------------------
static void set_state(web_state_t st)
{
    s_status.state = st;
    s_rev++;
}

static void set_activity(const char *fmt, const char *arg, int progress)
{
    snprintf(s_status.activity, sizeof(s_status.activity), fmt, arg);
    s_status.progress = progress;
    s_rev++;
}

static void set_progress(int progress)
{
    if (s_status.progress != progress) {
        s_status.progress = progress;
        s_rev++;
    }
}

static void fail(const char *msg, int err)
{
    printf("web: %s (error %d)\n", msg, err);
    snprintf(s_status.error, sizeof(s_status.error), "%s (error %d)", msg, err);
    set_state(WEB_FAILED);
}

uint32_t webserver_status(web_status_t *out)
{
    *out = s_status;
    return s_rev;
}

uint32_t webserver_changes(void)
{
    return s_changes;
}

bool webserver_take_flash_request(char *path, size_t len)
{
    bool pending = s_flash_pending;
    if (pending) {
        strlcpy(path, s_flash_path, len);
        s_flash_pending = false;
    }
    return pending;
}

// ---- paths ------------------------------------------------------------------------------------
static void url_decode(char *s)
{
    char *out = s;
    for (; *s; s++) {
        if (*s == '%' && s[1] && s[2]) {
            char hex[3] = { s[1], s[2], 0 };
            char *end;
            long v = strtol(hex, &end, 16);
            if (*end == '\0') {
                *out++ = (char)v;
                s += 2;
                continue;
            }
        }
        *out++ = *s;
    }
    *out = '\0';
}

// A card path like "/apps/x.uf2": absolute, no "." / ".." / empty parts, no control characters.
static bool path_ok(const char *p)
{
    if (p[0] != '/') {
        return false;
    }
    for (const char *c = p; *c; c++) {
        if ((unsigned char)*c < 0x20 || *c == '\\' || *c == ':' || *c == '*' || *c == '?' ||
            *c == '"' || *c == '<' || *c == '>' || *c == '|') {
            return false;
        }
    }
    const char *part = p + 1;
    while (*part) {
        const char *end = strchr(part, '/');
        size_t n = end ? (size_t)(end - part) : strlen(part);
        if ((n == 0 && end) || (n == 1 && part[0] == '.') || (n == 2 && part[0] == '.' && part[1] == '.')) {
            return false;
        }
        if (!end) {
            break;
        }
        part = end + 1;
    }
    return true;
}

// Copy the raw value of `key` from a query string ("a=1&b=2").
static bool query_value(const char *query, const char *key, char *out, size_t len)
{
    size_t klen = strlen(key);
    for (const char *p = query; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            size_t n = strcspn(v, "&");
            if (n >= len) {
                return false;
            }
            memcpy(out, v, n);
            out[n] = '\0';
            return true;
        }
    }
    return false;
}

// Read query parameter `key` as a card path. The root comes back as "" (SDCARD_MOUNT_POINT).
static bool query_path(const char *query, const char *key, char *path, size_t len)
{
    char val[PARAM_LEN];
    if (!query_value(query, key, val, sizeof(val))) {
        return false;
    }
    url_decode(val);
    size_t n = strlen(val);
    while (n > 1 && val[n - 1] == '/') {
        val[--n] = '\0';
    }
    if (!path_ok(val) || n >= len) {
        return false;
    }
    snprintf(path, len, "%s%s", SDCARD_MOUNT_POINT, strcmp(val, "/") == 0 ? "" : val);
    return true;
}

static long query_long(const char *query, const char *key, long def)
{
    char val[24];
    return query_value(query, key, val, sizeof(val)) ? strtol(val, NULL, 10) : def;
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// ---- FAT timestamps (kept as UTC, like the ESP32 build) ----------------------------------------
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + doe - 719468;
}

static int64_t fat_to_unix(WORD fdate, WORD ftime)
{
    int64_t days = days_from_civil(1980 + (fdate >> 9), (fdate >> 5) & 15, fdate & 31);
    return days * 86400 + (ftime >> 11) * 3600 + ((ftime >> 5) & 63) * 60 + (ftime & 31) * 2;
}

static void unix_to_fat(int64_t t, WORD *fdate, WORD *ftime)
{
    int64_t z = t / 86400 + 719468;
    int secs = (int)(t % 86400);
    int era = (int)((z >= 0 ? z : z - 146096) / 146097);
    unsigned doe = (unsigned)(z - (int64_t)era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = (int)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    *fdate = (WORD)(((y - 1980) << 9) | (m << 5) | d);
    *ftime = (WORD)(((secs / 3600) << 11) | (((secs / 60) % 60) << 5) | ((secs % 60) / 2));
}

static const char *fr_text(FRESULT fr)
{
    switch (fr) {
    case FR_NO_FILE: return "No such file or directory";
    case FR_NO_PATH: return "No such file or directory";
    case FR_EXIST: return "File exists";
    case FR_DENIED: return "Access denied (folder not empty or card full?)";
    case FR_WRITE_PROTECTED: return "Card is write-protected";
    case FR_INVALID_NAME: return "Invalid name";
    case FR_NOT_READY:
    case FR_DISK_ERR: return "SD card error";
    default: return "Error";
    }
}

static const char *fr_status(FRESULT fr)
{
    return fr == FR_NO_FILE || fr == FR_NO_PATH ? "404 Not Found"
           : fr == FR_EXIST                      ? "409 Conflict"
                                                 : "500 Internal Server Error";
}

// Append `s` as a JSON string literal.
static void json_str(char *out, size_t len, const char *s)
{
    size_t o = strlen(out);
    if (o + 2 >= len) {
        return;
    }
    out[o++] = '"';
    for (; *s && o + 8 < len; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20) {
            o += snprintf(out + o, len - o, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o++] = '"';
    out[o] = '\0';
}

// ---- connections ------------------------------------------------------------------------------
typedef enum { ST_HEADER, ST_BODY, ST_UPLOAD, ST_SEND } conn_state_t;
typedef enum { SRC_NONE, SRC_MEM, SRC_FILE, SRC_LIST } src_t;

typedef struct conn {
    struct conn *next;
    struct tcp_pcb *pcb;
    conn_state_t state;
    int idle;

    char hdr[HDR_MAX + 1];
    size_t hdr_len;
    char *method, *target, *query;
    uint32_t content_len, received;
    char body[BODY_MAX];
    size_t body_len;

    // A connection either reads/writes one file or lists one folder, never both.
    union {
        FIL file;
        struct {
            DIR dir;
            FILINFO fi;
        };
    };

    // upload
    bool file_open;
    bool uploading;
    const char *upload_err; // why the upload failed: answered once the body is drained
    const char *upload_status;
    FRESULT upload_fr;
    char path[PATH_LEN];

    // response
    char out[OUT_MAX];
    size_t out_len, out_off;
    src_t src;
    bool src_done;
    const char *mem;
    size_t mem_len;
    bool dir_open, first_entry;
} conn_t;
_Static_assert(sizeof(conn_t) <= 4700, "two connections must fit in the heap");

static conn_t *s_conns;
static int s_conn_count;

static void conn_free(conn_t *c, bool aborted)
{
    if (c->file_open) {
        f_close(&c->file);
        c->file_open = false;
        if (c->uploading) {
            f_unlink(sd_path(c->path)); // never leave half a file behind
            set_activity("Upload of %.40s failed", base_name(c->path), -1);
        } else if (aborted) {
            set_activity("Download of %.45s stopped", base_name(c->path), -1);
        }
    }
    if (c->dir_open) {
        f_closedir(&c->dir);
    }
    for (conn_t **p = &s_conns; *p; p = &(*p)->next) {
        if (*p == c) {
            *p = c->next;
            break;
        }
    }
    s_conn_count--;
    free(c);
}

// Returns ERR_ABRT if the pcb was aborted (lwIP callbacks must then return that).
static err_t conn_close(conn_t *c, bool abort)
{
    err_t ret = ERR_OK;
    struct tcp_pcb *pcb = c->pcb;
    if (pcb) {
        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_sent(pcb, NULL);
        tcp_err(pcb, NULL);
        tcp_poll(pcb, NULL, 0);
        if (abort || tcp_close(pcb) != ERR_OK) {
            tcp_abort(pcb);
            ret = ERR_ABRT;
        }
    }
    conn_free(c, abort);
    return ret;
}

// Start a response: the status line and headers go into the output buffer.
static void begin_response(conn_t *c, const char *status, const char *type, long length, const char *extra)
{
    c->out_off = 0;
    c->out_len = snprintf(c->out, sizeof(c->out),
                          "HTTP/1.1 %s\r\nContent-Type: %s\r\nConnection: close\r\n%s", status, type,
                          extra ? extra : "");
    if (length >= 0) {
        c->out_len += snprintf(c->out + c->out_len, sizeof(c->out) - c->out_len, "Content-Length: %ld\r\n",
                               length);
    }
    c->out_len += snprintf(c->out + c->out_len, sizeof(c->out) - c->out_len, "\r\n");
    c->state = ST_SEND;
    c->src = SRC_NONE;
    c->src_done = true;
}

static void send_text(conn_t *c, const char *status, const char *type, const char *body)
{
    size_t n = strlen(body);
    begin_response(c, status, type, (long)n, "Cache-Control: no-store\r\n");
    if (c->out_len + n > sizeof(c->out)) {
        n = sizeof(c->out) - c->out_len;
    }
    memcpy(c->out + c->out_len, body, n);
    c->out_len += n;
}

static void send_error(conn_t *c, const char *status, const char *msg)
{
    send_text(c, status, "text/plain", msg);
}

static void send_fr(conn_t *c, const char *what, FRESULT fr)
{
    char msg[96];
    snprintf(msg, sizeof(msg), "%s: %s", what, fr_text(fr));
    send_error(c, fr_status(fr), msg);
}

static void send_ok(conn_t *c)
{
    send_text(c, "200 OK", "application/json", "{\"ok\":true}");
}

// Refill the (drained) output buffer from the response source.
static void refill(conn_t *c)
{
    c->out_off = c->out_len = 0;
    switch (c->src) {
    case SRC_MEM: {
        size_t n = c->mem_len < sizeof(c->out) ? c->mem_len : sizeof(c->out);
        memcpy(c->out, c->mem, n);
        c->mem += n;
        c->mem_len -= n;
        c->out_len = n;
        c->src_done = c->mem_len == 0;
        break;
    }
    case SRC_FILE: {
        UINT n = 0;
        if (f_read(&c->file, c->out, sizeof(c->out), &n) != FR_OK || n == 0) {
            bool complete = f_eof(&c->file);
            f_close(&c->file);
            c->file_open = false;
            c->src_done = true;
            set_activity(complete ? "Sent %.55s" : "Download of %.45s stopped", base_name(c->path), -1);
        }
        c->out_len = n;
        break;
    }
    case SRC_LIST:
        // one entry needs at most ~2 * FF_MAX_LFN + 64 bytes
        while (c->out_len + 2 * FF_MAX_LFN + 80 < sizeof(c->out)) {
            if (f_readdir(&c->dir, &c->fi) != FR_OK || !c->fi.fname[0]) {
                f_closedir(&c->dir);
                c->dir_open = false;
                memcpy(c->out + c->out_len, "]}", 2);
                c->out_len += 2;
                c->src_done = true;
                break;
            }
            bool is_dir = c->fi.fattrib & AM_DIR;
            char *o = c->out + c->out_len;
            size_t room = sizeof(c->out) - c->out_len;
            snprintf(o, room, "%s{\"n\":", c->first_entry ? "" : ",");
            json_str(o, room, c->fi.fname);
            size_t used = strlen(o);
            used += snprintf(o + used, room - used, ",\"d\":%d,\"s\":%lu,\"t\":%lld}", is_dir ? 1 : 0,
                             is_dir ? 0UL : (unsigned long)c->fi.fsize,
                             (long long)fat_to_unix(c->fi.fdate, c->fi.ftime));
            c->out_len += used;
            c->first_entry = false;
        }
        break;
    default:
        c->src_done = true;
        break;
    }
}

// Queue as much of the response as lwIP takes; close once everything is queued.
static err_t pump(conn_t *c)
{
    if (c->state != ST_SEND) {
        return ERR_OK;
    }
    for (;;) {
        if (c->out_off == c->out_len) {
            if (c->src_done) {
                tcp_output(c->pcb);
                return conn_close(c, false);
            }
            refill(c);
            continue;
        }
        u16_t room = tcp_sndbuf(c->pcb);
        if (room == 0 || tcp_sndqueuelen(c->pcb) + 2 >= TCP_SND_QUEUELEN) {
            break;
        }
        size_t n = c->out_len - c->out_off;
        if (n > room) {
            n = room;
        }
        err_t err = tcp_write(c->pcb, c->out + c->out_off, (u16_t)n, TCP_WRITE_FLAG_COPY);
        if (err == ERR_MEM) {
            break;
        }
        if (err != ERR_OK) {
            return conn_close(c, true);
        }
        c->out_off += n;
    }
    tcp_output(c->pcb);
    return ERR_OK;
}

// ---- handlers ---------------------------------------------------------------------------------
static void index_get(conn_t *c)
{
    begin_response(c, "200 OK", "text/html; charset=utf-8", index_html_len,
                   "Cache-Control: no-cache\r\nContent-Encoding: gzip\r\n");
    c->src = SRC_MEM;
    c->mem = index_html;
    c->mem_len = index_html_len;
    c->src_done = false;
}

static void list_get(conn_t *c)
{
    if (!query_path(c->query, "path", c->path, sizeof(c->path))) {
        send_error(c, "400 Bad Request", "bad path");
        return;
    }
    FRESULT fr = f_opendir(&c->dir, sd_path(c->path));
    if (fr != FR_OK) {
        send_fr(c, "Can't open folder", fr);
        return;
    }
    c->dir_open = true;
    begin_response(c, "200 OK", "application/json", -1, "Cache-Control: no-store\r\n");
    char *o = c->out + c->out_len;
    size_t room = sizeof(c->out) - c->out_len;
    snprintf(o, room, "{\"path\":");
    json_str(o, room, sd_path(c->path));
    strlcat(o, ",\"entries\":[", room);
    c->out_len += strlen(o);
    c->src = SRC_LIST;
    c->src_done = false;
    c->first_entry = true;
}

static void info_get(conn_t *c)
{
    uint64_t total = 0, free_bytes = 0;
    sdcard_space(&total, &free_bytes);
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"total\":%llu,\"free\":%llu,\"ap\":%s,\"ssid\":", total, free_bytes,
             s_status.ap_mode ? "true" : "false");
    json_str(buf, sizeof(buf), s_status.ssid);
    strlcat(buf, "}", sizeof(buf));
    send_text(c, "200 OK", "application/json", buf);
}

static const char *mime_type(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) {
        return "application/octet-stream";
    }
    static const struct { const char *ext, *type; } types[] = {
        { ".txt", "text/plain; charset=utf-8" }, { ".md", "text/plain; charset=utf-8" },
        { ".log", "text/plain; charset=utf-8" }, { ".ini", "text/plain; charset=utf-8" },
        { ".cfg", "text/plain; charset=utf-8" }, { ".csv", "text/plain; charset=utf-8" },
        { ".py", "text/plain; charset=utf-8" },  { ".bas", "text/plain; charset=utf-8" },
        { ".json", "application/json" },         { ".htm", "text/html" },
        { ".html", "text/html" },                { ".png", "image/png" },
        { ".jpg", "image/jpeg" },                { ".jpeg", "image/jpeg" },
        { ".gif", "image/gif" },                 { ".bmp", "image/bmp" },
        { ".pdf", "application/pdf" },           { ".wav", "audio/wav" },
        { ".mp3", "audio/mpeg" },
    };
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        if (strcasecmp(dot, types[i].ext) == 0) {
            return types[i].type;
        }
    }
    return "application/octet-stream";
}

static void file_get(conn_t *c)
{
    if (!query_path(c->query, "path", c->path, sizeof(c->path))) {
        send_error(c, "400 Bad Request", "bad path");
        return;
    }
    FRESULT fr = f_open(&c->file, sd_path(c->path), FA_READ);
    if (fr != FR_OK) {
        send_fr(c, "Can't open file", fr);
        return;
    }
    c->file_open = true;
    const char *name = base_name(c->path);
    char extra[PATH_LEN + 80] = "Cache-Control: no-store\r\n";
    if (query_long(c->query, "dl", 0)) {
        // the name is already in the URL; strip quotes so the header stays well-formed
        char clean[PATH_LEN];
        size_t o = 0;
        for (const char *ch = name; *ch && o + 1 < sizeof(clean); ch++) {
            clean[o++] = *ch == '"' ? '_' : *ch;
        }
        clean[o] = '\0';
        size_t n = strlen(extra);
        snprintf(extra + n, sizeof(extra) - n, "Content-Disposition: attachment; filename=\"%s\"\r\n", clean);
    }
    begin_response(c, "200 OK", mime_type(name), (long)f_size(&c->file), extra);
    c->src = SRC_FILE;
    c->src_done = false;
    set_activity("Sending %.50s", name, -1);
}

// Called once the headers of an upload are in; the body is written as it arrives.
static void upload_begin(conn_t *c)
{
    c->state = ST_UPLOAD;
    if (!query_path(c->query, "path", c->path, sizeof(c->path)) || !c->path[0]) {
        c->upload_status = "400 Bad Request";
        c->upload_err = "bad path";
        return;
    }
    FILINFO fi;
    if (f_stat(c->path, &fi) == FR_OK && (fi.fattrib & AM_DIR)) {
        c->upload_status = "409 Conflict";
        c->upload_err = "a folder with that name exists";
        return;
    }
    FRESULT fr = f_open(&c->file, c->path, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        c->upload_status = fr_status(fr);
        c->upload_err = "Can't create file";
        c->upload_fr = fr;
        return;
    }
    c->file_open = true;
    c->uploading = true;
    printf("web: upload %s (%lu bytes)\n", c->path, (unsigned long)c->content_len);
    set_activity("Receiving %.50s", base_name(c->path), 0);
}

static void upload_data(conn_t *c, const void *data, size_t len)
{
    if (!c->uploading) {
        return; // failed already: just drain the body
    }
    UINT n = 0;
    FRESULT fr = f_write(&c->file, data, (UINT)len, &n);
    if (fr != FR_OK || n != len) {
        c->upload_status = "500 Internal Server Error";
        c->upload_err = fr == FR_OK ? "SD card full" : "write failed";
        f_close(&c->file);
        c->file_open = false;
        c->uploading = false;
        f_unlink(c->path);
        set_activity("Upload of %.40s failed", base_name(c->path), -1);
        return;
    }
    set_progress(c->content_len ? (int)((uint64_t)c->received * 100 / c->content_len) : 100);
}

static void upload_end(conn_t *c)
{
    if (c->uploading) {
        c->uploading = false;
        c->file_open = false;
        if (f_close(&c->file) != FR_OK) {
            f_unlink(c->path);
            c->upload_status = "500 Internal Server Error";
            c->upload_err = "write failed";
            set_activity("Upload of %.40s failed", base_name(c->path), -1);
        } else {
            long mtime = query_long(c->query, "mtime", 0);
            if (mtime > 315532800) { // FAT can't store dates before 1980
                FILINFO fi = { 0 };
                unix_to_fat(mtime, &fi.fdate, &fi.ftime);
                f_utime(c->path, &fi);
            }
            set_activity("Received %.50s", base_name(c->path), -1);
            s_changes++;
        }
    }
    if (c->upload_fr != FR_OK) {
        send_fr(c, c->upload_err, c->upload_fr);
        return;
    }
    if (c->upload_err) {
        send_error(c, c->upload_status, c->upload_err);
        return;
    }
    send_ok(c);
}

static void mkdir_post(conn_t *c)
{
    if (!query_path(c->query, "path", c->path, sizeof(c->path)) || !c->path[0]) {
        send_error(c, "400 Bad Request", "bad path");
        return;
    }
    FRESULT fr = f_mkdir(c->path);
    if (fr != FR_OK) {
        send_fr(c, "Can't create folder", fr);
        return;
    }
    set_activity("Created folder %.45s", base_name(c->path), -1);
    s_changes++;
    send_ok(c);
}

// Remove a file, or a folder and everything in it. `path` is extended in place while descending.
static FRESULT remove_tree(char *path, size_t cap, int depth)
{
    static FILINFO fi; // only needed until the name is copied: one for all levels
    FRESULT fr = f_stat(path, &fi);
    if (fr != FR_OK) {
        return fr;
    }
    if (!(fi.fattrib & AM_DIR)) {
        return f_unlink(path);
    }
    if (depth > 16) {
        return FR_INVALID_NAME;
    }
    size_t len = strlen(path);
    for (;;) {
        // reopen after each removal: deleting while iterating a FAT directory skips entries
        DIR dir;
        if ((fr = f_opendir(&dir, path)) != FR_OK) {
            return fr;
        }
        fr = f_readdir(&dir, &fi);
        f_closedir(&dir);
        if (fr != FR_OK) {
            return fr;
        }
        if (!fi.fname[0]) {
            break;
        }
        if (len + 1 + strlen(fi.fname) + 1 > cap) {
            return FR_INVALID_NAME;
        }
        snprintf(path + len, cap - len, "/%s", fi.fname);
        fr = remove_tree(path, cap, depth + 1);
        path[len] = '\0';
        if (fr != FR_OK) {
            return fr;
        }
    }
    return f_unlink(path);
}

static void delete_post(conn_t *c)
{
    if (!query_path(c->query, "path", c->path, sizeof(c->path)) || !c->path[0]) {
        send_error(c, "400 Bad Request", "bad path");
        return;
    }
    char name[64];
    strlcpy(name, base_name(c->path), sizeof(name));
    FRESULT fr = remove_tree(c->path, sizeof(c->path), 0);
    if (fr != FR_OK) {
        send_fr(c, "Can't delete", fr);
        return;
    }
    set_activity("Deleted %.55s", name, -1);
    s_changes++;
    send_ok(c);
}

static void rename_post(conn_t *c)
{
    char to[PATH_LEN];
    if (!query_path(c->query, "from", c->path, sizeof(c->path)) || !query_path(c->query, "to", to, sizeof(to)) ||
        !c->path[0] || !to[0]) {
        send_error(c, "400 Bad Request", "bad path");
        return;
    }
    FILINFO fi;
    if (f_stat(to, &fi) == FR_OK) {
        send_error(c, "409 Conflict", "something with that name already exists");
        return;
    }
    FRESULT fr = f_rename(c->path, to);
    if (fr != FR_OK) {
        send_fr(c, "Can't rename", fr);
        return;
    }
    set_activity("Renamed to %.50s", base_name(to), -1);
    s_changes++;
    send_ok(c);
}

static void flash_post(conn_t *c)
{
    FILINFO fi;
    if (!query_path(c->query, "path", c->path, sizeof(c->path))) {
        send_error(c, "400 Bad Request", "bad path");
        return;
    }
    if (!c->path[0] || f_stat(c->path, &fi) != FR_OK || (fi.fattrib & AM_DIR)) {
        send_error(c, "404 Not Found", "no such file");
        return;
    }
    strlcpy(s_flash_path, c->path, sizeof(s_flash_path));
    s_flash_pending = true;
    s_rev++;
    printf("web: flash requested: %s\n", c->path);
    send_ok(c);
}

static void wifi_post(conn_t *c)
{
    if (c->content_len == 0 || c->content_len >= sizeof(c->body)) {
        send_error(c, "400 Bad Request", "send \"ssid\\npassword\"");
        return;
    }
    char *body = c->body;
    body[c->body_len] = '\0';
    char *nl = strchr(body, '\n');
    size_t ssid_len = nl ? (size_t)(nl - body) : strlen(body);
    size_t pass_len = nl ? strlen(nl + 1) : 0;
    if (ssid_len == 0 || ssid_len > 32 || pass_len > 64 || (pass_len > 0 && pass_len < 8)) {
        send_error(c, "400 Bad Request", "The network name must be 1-32 characters, and the password 8-64 (or empty)");
        return;
    }
    body[ssid_len] = '\0';
    if (!web_wifi_save(body, nl ? nl + 1 : "")) {
        send_error(c, "500 Internal Server Error", "could not save the settings");
        return;
    }
    set_activity("%s", "Saved Wi-Fi settings", -1);
    send_ok(c);
}

// The full request (headers and any small body) is in.
static void dispatch(conn_t *c)
{
    const char *t = c->target;
    bool get = strcmp(c->method, "GET") == 0, post = strcmp(c->method, "POST") == 0;
    if (get && (strcmp(t, "/") == 0 || strcmp(t, "/index.html") == 0)) {
        index_get(c);
    } else if (get && strcmp(t, "/api/list") == 0) {
        list_get(c);
    } else if (get && strcmp(t, "/api/info") == 0) {
        info_get(c);
    } else if (get && strcmp(t, "/api/file") == 0) {
        file_get(c);
    } else if (post && strcmp(t, "/api/mkdir") == 0) {
        mkdir_post(c);
    } else if (post && strcmp(t, "/api/delete") == 0) {
        delete_post(c);
    } else if (post && strcmp(t, "/api/rename") == 0) {
        rename_post(c);
    } else if (post && strcmp(t, "/api/flash") == 0) {
        flash_post(c);
    } else if (post && strcmp(t, "/api/wifi") == 0) {
        wifi_post(c);
    } else {
        send_error(c, "404 Not Found", "not found");
    }
}

// Parse the request line and the headers we care about. False if malformed.
static bool parse_header(conn_t *c)
{
    char *line_end = strstr(c->hdr, "\r\n");
    if (!line_end) {
        return false;
    }
    *line_end = '\0';
    c->method = c->hdr;
    char *sp = strchr(c->hdr, ' ');
    if (!sp) {
        return false;
    }
    *sp = '\0';
    c->target = sp + 1;
    sp = strchr(c->target, ' ');
    if (sp) {
        *sp = '\0';
    }
    char *q = strchr(c->target, '?');
    if (q) {
        *q = '\0';
        c->query = q + 1;
    } else {
        c->query = "";
    }
    c->content_len = 0;
    for (char *h = line_end + 2; *h; ) {
        char *end = strstr(h, "\r\n");
        if (!end) {
            break;
        }
        *end = '\0';
        if (strncasecmp(h, "Content-Length:", 15) == 0) {
            c->content_len = strtoul(h + 15, NULL, 10);
        }
        h = end + 2;
    }
    return true;
}

// Feed received bytes into the connection's state machine.
static void consume(conn_t *c, const uint8_t *data, size_t len)
{
    while (len > 0) {
        switch (c->state) {
        case ST_HEADER: {
            size_t take = HDR_MAX - c->hdr_len;
            if (take == 0) {
                send_error(c, "431 Request Header Fields Too Large", "request too large");
                return;
            }
            if (take > len) {
                take = len;
            }
            memcpy(c->hdr + c->hdr_len, data, take);
            c->hdr_len += take;
            c->hdr[c->hdr_len] = '\0';
            char *end = strstr(c->hdr, "\r\n\r\n");
            if (!end) {
                data += take;
                len -= take;
                break;
            }
            // anything after the blank line is body
            size_t used = (size_t)(end + 4 - c->hdr) - (c->hdr_len - take);
            data += used;
            len -= used;
            end[2] = '\0';
            if (!parse_header(c)) {
                send_error(c, "400 Bad Request", "bad request");
                return;
            }
            if (strcmp(c->method, "POST") == 0 && strcmp(c->target, "/api/upload") == 0) {
                upload_begin(c);
                if (c->content_len == 0) {
                    upload_end(c);
                }
            } else if (c->content_len > 0) {
                c->state = ST_BODY;
            } else {
                dispatch(c);
            }
            break;
        }
        case ST_BODY:
        case ST_UPLOAD: {
            size_t take = c->content_len - c->received;
            if (take > len) {
                take = len;
            }
            if (c->state == ST_UPLOAD) {
                c->received += take;
                upload_data(c, data, take);
            } else {
                if (c->body_len + take < sizeof(c->body)) {
                    memcpy(c->body + c->body_len, data, take);
                    c->body_len += take;
                }
                c->received += take;
            }
            data += take;
            len -= take;
            if (c->received >= c->content_len) {
                if (c->state == ST_UPLOAD) {
                    upload_end(c);
                } else {
                    dispatch(c);
                }
            }
            break;
        }
        case ST_SEND:
            return; // ignore anything after the request
        }
    }
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    conn_t *c = arg;
    if (!c) {
        if (p) {
            tcp_recved(pcb, p->tot_len);
            pbuf_free(p);
        }
        return ERR_OK;
    }
    if (!p) { // the browser closed its side
        if (c->state == ST_SEND) {
            return ERR_OK; // half-close: keep sending
        }
        return conn_close(c, true);
    }
    c->idle = 0;
    for (struct pbuf *q = p; q; q = q->next) {
        consume(c, q->payload, q->len);
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return c->state == ST_SEND ? pump(c) : ERR_OK;
}

static err_t on_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    conn_t *c = arg;
    if (c) {
        c->idle = 0;
        return pump(c);
    }
    return ERR_OK;
}

static err_t on_poll(void *arg, struct tcp_pcb *pcb)
{
    conn_t *c = arg;
    if (!c) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    if (++c->idle > IDLE_POLLS) {
        return conn_close(c, true);
    }
    return pump(c);
}

static void on_err(void *arg, err_t err)
{
    conn_t *c = arg;
    if (c) {
        c->pcb = NULL; // already freed by lwIP
        conn_free(c, true);
    }
}

static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err)
{
    if (err != ERR_OK || !pcb) {
        return ERR_VAL;
    }
    conn_t *c = s_conn_count < MAX_CONNS ? calloc(1, sizeof(conn_t)) : NULL;
    if (!c) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    c->pcb = pcb;
    c->state = ST_HEADER;
    c->next = s_conns;
    s_conns = c;
    s_conn_count++;
    tcp_arg(pcb, c);
    tcp_recv(pcb, on_recv);
    tcp_sent(pcb, on_sent);
    tcp_err(pcb, on_err);
    tcp_poll(pcb, on_poll, POLL_INTERVAL);
    return ERR_OK;
}

// ---- saved network ----------------------------------------------------------------------------
bool web_wifi_load(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    ssid[0] = pass[0] = '\0';
    if (!config_get("wifi_ssid", ssid, ssid_len) || !ssid[0]) {
        return false;
    }
    if (!config_get("wifi_pass", pass, pass_len)) {
        pass[0] = '\0';
    }
    return true;
}

bool web_wifi_save(const char *ssid, const char *pass)
{
    if (strlen(ssid) > 32 || strlen(pass) > 64 || (pass[0] && strlen(pass) < 8)) {
        return false;
    }
    if (!ssid[0]) { // forget the network
        return config_set("wifi_ssid", "") && config_set("wifi_pass", "");
    }
    return config_set("wifi_ssid", ssid) && config_set("wifi_pass", pass);
}

// ---- Wi-Fi ------------------------------------------------------------------------------------
static bool radio_init(void)
{
    if (s_radio_ready) {
        return true;
    }
    int err = cyw43_arch_init();
    if (err) {
        printf("web: cyw43_arch_init failed (%d)\n", err);
        return false;
    }
    mdns_resp_init();
    s_radio_ready = true;
    return true;
}

typedef struct {
    web_ap_t *out;
    int max, count;
} scan_ctx_t;

static int scan_result(void *env, const cyw43_ev_scan_result_t *r)
{
    scan_ctx_t *ctx = env;
    char name[33];
    size_t n = r->ssid_len < 32 ? r->ssid_len : 32;
    memcpy(name, r->ssid, n);
    name[n] = '\0';
    if (!name[0]) {
        return 0;
    }
    int8_t rssi = r->rssi < -128 ? -128 : r->rssi > 0 ? 0 : (int8_t)r->rssi;
    for (int i = 0; i < ctx->count; i++) {
        if (strcmp(ctx->out[i].ssid, name) == 0) { // keep the strongest of each name
            if (rssi > ctx->out[i].rssi) {
                ctx->out[i].rssi = rssi;
            }
            return 0;
        }
    }
    if (ctx->count < ctx->max) {
        web_ap_t *ap = &ctx->out[ctx->count++];
        strlcpy(ap->ssid, name, sizeof(ap->ssid));
        ap->rssi = rssi;
        ap->secure = r->auth_mode != 0;
    }
    return 0;
}

static int cmp_rssi(const void *a, const void *b)
{
    return ((const web_ap_t *)b)->rssi - ((const web_ap_t *)a)->rssi;
}

int web_wifi_scan(web_ap_t *out, int max)
{
    if (s_status.state != WEB_OFF || !radio_init()) {
        return -1;
    }
    cyw43_arch_enable_sta_mode();
    scan_ctx_t ctx = { .out = out, .max = max };
    cyw43_wifi_scan_options_t opts = { 0 };
    int count = -1;
    if (cyw43_wifi_scan(&cyw43_state, &opts, &ctx, scan_result) == 0) {
        absolute_time_t until = make_timeout_time_ms(10000);
        while (cyw43_wifi_scan_active(&cyw43_state) && !time_reached(until)) {
            cyw43_arch_poll();
            sleep_ms(5);
        }
        count = ctx.count;
        qsort(out, count, sizeof(*out), cmp_rssi);
    }
    cyw43_arch_disable_sta_mode();
    printf("web: scan found %d networks\n", count);
    return count;
}

static void on_ap_join(void)
{
    set_activity("%s", "A device joined the access point", -1);
}

// The AP password is random, made once and kept in the settings, so a phone that saved the
// network can rejoin next time.
static void ap_password(char *out, size_t len)
{
    if (config_get("ap_pass", out, len) && strlen(out) >= 8) {
        return;
    }
    static const char chars[] = "abcdefghjkmnpqrstuvwxyz23456789"; // no 0/o, 1/l/i
    int i;
    for (i = 0; i < 10 && i + 1 < (int)len; i++) {
        out[i] = chars[get_rand_32() % (sizeof(chars) - 1)];
    }
    out[i] = '\0';
    config_set("ap_pass", out);
}

static bool start_http(struct netif *netif)
{
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb) {
        return false;
    }
    if (tcp_bind(pcb, IP_ANY_TYPE, HTTP_PORT) != ERR_OK) {
        tcp_close(pcb);
        return false;
    }
    s_listen = tcp_listen_with_backlog(pcb, MAX_CONNS);
    if (!s_listen) {
        tcp_close(pcb);
        return false;
    }
    tcp_accept(s_listen, on_accept);

    s_netif = netif;
    if (mdns_resp_add_netif(netif, WEB_HOSTNAME) == ERR_OK) {
        s_mdns = true;
        mdns_resp_add_service(netif, "PicoMultiLoader", "_http", DNSSD_PROTO_TCP, HTTP_PORT, NULL, NULL);
        mdns_resp_announce(netif);
    }
    return true;
}

static void server_up(struct netif *netif)
{
    if (!start_http(netif)) {
        fail("Could not start the web server", -1);
        return;
    }
    if (!s_status.ap_mode || !s_status.activity[0]) {
        set_activity("%s", "Waiting for a browser", -1); // else keep why joining failed
    }
    set_state(WEB_RUNNING);
    printf("web: running at http://%s/\n", s_status.ip);
}

static void start_access_point(void)
{
    uint8_t mac[6] = { 0 };
    cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_AP, mac);
    char ssid[33], pass[16];
    snprintf(ssid, sizeof(ssid), AP_SSID_PREFIX "-%02X%02X", mac[4], mac[5]);
    ap_password(pass, sizeof(pass));
    cyw43_arch_enable_ap_mode(ssid, pass, CYW43_AUTH_WPA2_AES_PSK);

    struct netif *netif = &cyw43_state.netif[CYW43_ITF_AP];
    s_status.ap_mode = true;
    strlcpy(s_status.ssid, ssid, sizeof(s_status.ssid));
    strlcpy(s_status.password, pass, sizeof(s_status.password));
    strlcpy(s_status.ip, ip4addr_ntoa(netif_ip4_addr(netif)), sizeof(s_status.ip));
    s_rev++;
    printf("web: access point \"%s\" password %s, %s\n", ssid, pass, s_status.ip);
    if (!dhcpserver_start(netif, on_ap_join)) {
        fail("Could not start the DHCP server", -1);
        return;
    }
    server_up(netif);
}

static int join(void)
{
    return cyw43_arch_wifi_connect_async(s_join_ssid, s_join_pass[0] ? s_join_pass : NULL,
                                         s_join_pass[0] ? CYW43_AUTH_WPA2_MIXED_PSK : CYW43_AUTH_OPEN);
}

static void join_failed(const char *fmt)
{
    printf("web: could not join \"%s\"\n", s_join_ssid);
    set_activity(fmt, s_join_ssid, -1);
    cyw43_arch_disable_sta_mode();
    start_access_point();
}

static void start_step(void)
{
    if (!radio_init()) {
        fail("The CYW43 Wi-Fi chip did not start", -1);
        return;
    }
    if (!web_wifi_load(s_join_ssid, sizeof(s_join_ssid), s_join_pass, sizeof(s_join_pass))) {
        printf("web: no Wi-Fi network set up (F1), starting an access point\n");
        start_access_point();
        return;
    }
    s_status.ap_mode = false;
    strlcpy(s_status.ssid, s_join_ssid, sizeof(s_status.ssid));
    set_state(WEB_CONNECTING);
    printf("web: joining \"%s\"\n", s_join_ssid);
    cyw43_arch_enable_sta_mode();
    s_join_saw_nonet = false;
    s_join_deadline_us = time_us_64() + CONNECT_TIMEOUT_MS * 1000ull;
    s_join_retry_us = time_us_64() + 1000000;
    if (join() != 0) {
        join_failed("Could not join %.40s");
    }
}

static void join_step(void)
{
    int st = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    uint64_t now = time_us_64();
    if (st == CYW43_LINK_UP) {
        struct netif *netif = &cyw43_state.netif[CYW43_ITF_STA];
        strlcpy(s_status.ip, ip4addr_ntoa(netif_ip4_addr(netif)), sizeof(s_status.ip));
        s_link_check_us = now + 1000000;
        server_up(netif);
    } else if (st == CYW43_LINK_BADAUTH) {
        join_failed("Wrong password for %.36s?");
    } else if (now >= s_join_deadline_us || st == CYW43_LINK_FAIL) {
        join_failed(s_join_saw_nonet ? "%.34s not found (2.4 GHz only)" : "Could not join %.40s");
    } else if (st == CYW43_LINK_NONET && now >= s_join_retry_us) {
        s_join_saw_nonet = true; // keep trying until the timeout, the network may just be slow to answer
        s_join_retry_us = now + 1000000;
        join();
    }
}

// While running as a station: follow the link and rejoin if it drops.
static void link_step(void)
{
    uint64_t now = time_us_64();
    if (s_status.ap_mode || now < s_link_check_us) {
        return;
    }
    s_link_check_us = now + 1000000;
    int st = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    if (st == CYW43_LINK_UP) {
        const char *ip = ip4addr_ntoa(netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]));
        if (strcmp(ip, s_status.ip) != 0) {
            strlcpy(s_status.ip, ip, sizeof(s_status.ip));
            s_rev++;
            if (s_mdns) {
                mdns_resp_announce(s_netif);
            }
        }
    } else {
        if (s_status.ip[0]) {
            printf("web: lost the connection (%d)\n", st);
            s_status.ip[0] = '\0';
            s_rev++;
        }
        if (st != CYW43_LINK_JOIN && st != CYW43_LINK_NOIP && now >= s_join_retry_us) {
            s_join_retry_us = now + 5000000;
            join();
        }
    }
}

void webserver_poll(void)
{
    if (s_radio_ready) {
        cyw43_arch_poll();
    }
    switch (s_status.state) {
    case WEB_STARTING:
        if (time_us_64() >= s_start_at_us) { // let the UI show "starting" first: this blocks a moment
            start_step();
        }
        break;
    case WEB_CONNECTING:
        join_step();
        break;
    case WEB_RUNNING:
        link_step();
        break;
    default:
        break;
    }
}

bool webserver_start(void)
{
    if (s_status.state != WEB_OFF) {
        return false;
    }
    s_flash_pending = false;
    memset(&s_status, 0, sizeof(s_status));
    s_status.progress = -1;
    s_start_at_us = time_us_64() + 100000;
    set_state(WEB_STARTING);
    return true;
}

void webserver_stop(void)
{
    if (s_status.state == WEB_OFF) {
        return;
    }
    while (s_conns) {
        conn_close(s_conns, true);
    }
    if (s_listen) {
        tcp_close(s_listen);
        s_listen = NULL;
    }
    if (s_mdns) {
        mdns_resp_remove_netif(s_netif);
        s_mdns = false;
    }
    s_netif = NULL;
    dhcpserver_stop();
    if (s_radio_ready) {
        cyw43_arch_disable_sta_mode();
        cyw43_arch_disable_ap_mode();
        cyw43_arch_poll();
    }
    set_state(WEB_OFF);
    printf("web: stopped\n");
}
