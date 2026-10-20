// webserver.c - F4 mode: Wi-Fi through the ESP32-C6 (esp_wifi_remote + esp_hosted over SDIO) and
// a small REST API plus an embedded single-page file manager for the SD card.
//
//   GET  /                         the file manager page (main/web/index.html)
//   GET  /api/list?path=/dir       {"path","entries":[{"n":name,"d":is_dir,"s":size,"t":mtime}]}
//   GET  /api/info                 {"total","free","ssid","ap"} (bytes)
//   GET  /api/file?path=/f[&dl=1]  download / view a file
//   POST /api/upload?path=/f[&mtime=unix]   raw request body becomes the file (replaces it)
//   POST /api/mkdir?path=/dir
//   POST /api/delete?path=/f       files, or folders with everything in them
//   POST /api/rename?from=/a&to=/b
//   POST /api/flash?path=/f.bin    hand the file to the loader to flash and boot
//   POST /api/wifi                 body "ssid\npassword": the network to join (NVS, same as F1)
//
// Paths are relative to the SD card root, URL-encoded, and may not contain "." or ".." parts.
#include "webserver.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_netif.h"
#include "esp_vfs_fat.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs.h"
#include "sdcard.h"

static const char *TAG = "web";

#define CONNECT_TIMEOUT_MS 20000
#define AP_SSID_PREFIX     "PicoMultiLoader"
#define AP_CHANNEL         6
#define NVS_NS             "loader"
#define IO_BUF_SIZE        (16 * 1024)
#define PARAM_LEN          512
#define VFS_PATH_LEN       (PARAM_LEN + 16)

#define BIT_GOT_IP BIT0
#define BIT_STOP   BIT1
#define BIT_DONE   BIT2

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static SemaphoreHandle_t s_lock;
static EventGroupHandle_t s_events;
static web_status_t s_status = { .state = WEB_OFF, .progress = -1 };
static uint32_t s_rev;
static volatile uint32_t s_changes; // bumped whenever a browser changes the card
static bool s_wifi_ready, s_want_sta, s_stopping;
static int s_last_reason; // why the station last disconnected (wifi_err_reason_t)
static esp_netif_t *s_sta_netif, *s_ap_netif;
static httpd_handle_t s_httpd;
static bool s_mdns;
static char s_flash_path[VFS_PATH_LEN];
static bool s_flash_pending;

// ---- status -----------------------------------------------------------------------------------
#define LOCKED(code)                                  \
    do {                                              \
        xSemaphoreTake(s_lock, portMAX_DELAY);        \
        code;                                         \
        s_rev++;                                      \
        xSemaphoreGive(s_lock);                       \
    } while (0)

static void set_state(web_state_t st)
{
    LOCKED(s_status.state = st);
}

static void set_activity(const char *fmt, const char *arg, int progress)
{
    LOCKED({
        snprintf(s_status.activity, sizeof(s_status.activity), fmt, arg);
        s_status.progress = progress;
    });
}

static void set_progress(int progress)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status.progress != progress) {
        s_status.progress = progress;
        s_rev++;
    }
    xSemaphoreGive(s_lock);
}

static void fail(const char *msg, esp_err_t err)
{
    ESP_LOGE(TAG, "%s: %s", msg, esp_err_to_name(err));
    LOCKED({
        snprintf(s_status.error, sizeof(s_status.error), "%s (%s)", msg, esp_err_to_name(err));
        s_status.state = WEB_FAILED;
    });
}

uint32_t webserver_status(web_status_t *out)
{
    if (!s_lock) {
        *out = s_status;
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    uint32_t rev = s_rev;
    xSemaphoreGive(s_lock);
    return rev;
}

uint32_t webserver_changes(void)
{
    return s_changes;
}

bool webserver_take_flash_request(char *path, size_t len)
{
    if (!s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool pending = s_flash_pending;
    if (pending) {
        strlcpy(path, s_flash_path, len);
        s_flash_pending = false;
    }
    xSemaphoreGive(s_lock);
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

// A card path like "/apps/x.bin": absolute, no "." / ".." / empty parts, no control characters.
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

// Read query parameter `key` as a card path and turn it into a VFS path under /sdcard.
static bool query_path(httpd_req_t *req, const char *key, char *vfs, size_t len)
{
    char query[2 * PARAM_LEN + 64], val[PARAM_LEN];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, key, val, sizeof(val)) != ESP_OK) {
        return false;
    }
    url_decode(val);
    size_t n = strlen(val);
    while (n > 1 && val[n - 1] == '/') {
        val[--n] = '\0';
    }
    if (!path_ok(val)) {
        return false;
    }
    snprintf(vfs, len, "%s%s", SDCARD_MOUNT_POINT, strcmp(val, "/") == 0 ? "" : val);
    return true;
}

static long query_long(httpd_req_t *req, const char *key, long def)
{
    char query[2 * PARAM_LEN + 64], val[24];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, key, val, sizeof(val)) != ESP_OK) {
        return def;
    }
    return strtol(val, NULL, 10);
}

static const char *card_path(const char *vfs)
{
    const char *p = vfs + strlen(SDCARD_MOUNT_POINT);
    return *p ? p : "/";
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// ---- responses --------------------------------------------------------------------------------
static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, msg);
}

static esp_err_t send_errno(httpd_req_t *req, const char *what)
{
    char msg[96];
    snprintf(msg, sizeof(msg), "%s: %s", what, strerror(errno));
    return send_error(req, errno == ENOENT ? "404 Not Found" : errno == EEXIST ? "409 Conflict"
                                                                                : "500 Internal Server Error",
                      msg);
}

static esp_err_t send_ok(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
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

// ---- handlers ---------------------------------------------------------------------------------
static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t list_get(httpd_req_t *req)
{
    char vfs[VFS_PATH_LEN];
    if (!query_path(req, "path", vfs, sizeof(vfs))) {
        return send_error(req, "400 Bad Request", "bad path");
    }
    DIR *dir = opendir(vfs);
    if (!dir) {
        return send_errno(req, "Can't open folder");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    // entries are sent in chunks of a few KB: one chunk per entry is slow on big folders
    const size_t cap = 4096, entry_max = 1024;
    char *buf = malloc(cap);
    char *full = malloc(VFS_PATH_LEN + 260);
    if (!buf || !full) {
        free(buf);
        free(full);
        closedir(dir);
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    snprintf(buf, cap, "{\"path\":");
    json_str(buf, cap, card_path(vfs));
    strlcat(buf, ",\"entries\":[", cap);
    size_t used = strlen(buf);

    bool first = true;
    esp_err_t err = ESP_OK;
    struct dirent *de;
    while (err == ESP_OK && (de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.' && (de->d_name[1] == '\0' || (de->d_name[1] == '.' && de->d_name[2] == '\0'))) {
            continue;
        }
        snprintf(full, VFS_PATH_LEN + 260, "%s/%s", vfs, de->d_name);
        struct stat st = { 0 };
        stat(full, &st);
        bool is_dir = de->d_type == DT_DIR || S_ISDIR(st.st_mode);
        if (used + entry_max > cap) {
            err = httpd_resp_send_chunk(req, buf, used);
            used = 0;
        }
        char *e = buf + used;
        snprintf(e, entry_max, "%s{\"n\":", first ? "" : ",");
        json_str(e, entry_max, de->d_name);
        size_t o = strlen(e);
        snprintf(e + o, entry_max - o, ",\"d\":%d,\"s\":%lu,\"t\":%lld}", is_dir ? 1 : 0,
                 is_dir ? 0UL : (unsigned long)st.st_size, (long long)st.st_mtime);
        used += strlen(e);
        first = false;
    }
    closedir(dir);
    free(full);
    if (err == ESP_OK) {
        strlcpy(buf + used, "]}", cap - used);
        err = httpd_resp_send_chunk(req, buf, used + 2);
    }
    free(buf);
    return err == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : err;
}

static esp_err_t info_get(httpd_req_t *req)
{
    uint64_t total = 0, free_bytes = 0;
    esp_vfs_fat_info(SDCARD_MOUNT_POINT, &total, &free_bytes);
    web_status_t st;
    webserver_status(&st);
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"total\":%llu,\"free\":%llu,\"ap\":%s,\"ssid\":", total, free_bytes,
             st.ap_mode ? "true" : "false");
    json_str(buf, sizeof(buf), st.ssid);
    strlcat(buf, "}", sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, buf);
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

static esp_err_t file_get(httpd_req_t *req)
{
    char vfs[VFS_PATH_LEN];
    if (!query_path(req, "path", vfs, sizeof(vfs))) {
        return send_error(req, "400 Bad Request", "bad path");
    }
    int fd = open(vfs, O_RDONLY);
    if (fd < 0) {
        return send_errno(req, "Can't open file");
    }
    char *buf = malloc(IO_BUF_SIZE);
    if (!buf) {
        close(fd);
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    const char *name = base_name(vfs);
    httpd_resp_set_type(req, mime_type(name));
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char disp[300];
    if (query_long(req, "dl", 0)) {
        // the name is already in the URL; strip quotes so the header stays well-formed
        char clean[260];
        size_t o = 0;
        for (const char *c = name; *c && o + 1 < sizeof(clean); c++) {
            clean[o++] = *c == '"' ? '_' : *c;
        }
        clean[o] = '\0';
        snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", clean);
        httpd_resp_set_hdr(req, "Content-Disposition", disp);
    }

    set_activity("Sending %.50s", name, -1);
    esp_err_t err = ESP_OK;
    ssize_t n;
    while ((n = read(fd, buf, IO_BUF_SIZE)) > 0) {
        if ((err = httpd_resp_send_chunk(req, buf, n)) != ESP_OK || s_stopping) {
            break;
        }
    }
    close(fd);
    free(buf);
    if (err != ESP_OK || s_stopping) {
        set_activity("Download of %.45s stopped", name, -1);
        return ESP_FAIL;
    }
    set_activity("Sent %.55s", name, -1);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t upload_post(httpd_req_t *req)
{
    char vfs[VFS_PATH_LEN];
    if (!query_path(req, "path", vfs, sizeof(vfs)) || strcmp(vfs, SDCARD_MOUNT_POINT) == 0) {
        return send_error(req, "400 Bad Request", "bad path");
    }
    struct stat st;
    if (stat(vfs, &st) == 0 && S_ISDIR(st.st_mode)) {
        return send_error(req, "409 Conflict", "a folder with that name exists");
    }
    int fd = open(vfs, O_WRONLY | O_CREAT | O_TRUNC, 0664);
    if (fd < 0) {
        return send_errno(req, "Can't create file");
    }
    char *buf = malloc(IO_BUF_SIZE);
    if (!buf) {
        close(fd);
        unlink(vfs);
        return send_error(req, "500 Internal Server Error", "out of memory");
    }

    const char *name = base_name(vfs);
    size_t total = req->content_len, done = 0;
    set_activity("Receiving %.50s", name, 0);
    ESP_LOGI(TAG, "Upload %s (%u bytes)", vfs, (unsigned)total);
    const char *fail_msg = NULL;
    int timeouts = 0;
    while (done < total) {
        if (s_stopping) {
            fail_msg = "server stopping";
            break;
        }
        size_t want = total - done < IO_BUF_SIZE ? total - done : IO_BUF_SIZE;
        int n = httpd_req_recv(req, buf, want);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 4) {
            continue;
        }
        if (n <= 0) {
            fail_msg = "connection lost";
            break;
        }
        timeouts = 0;
        if (write(fd, buf, n) != n) {
            fail_msg = errno == ENOSPC ? "SD card full" : "write failed";
            break;
        }
        done += n;
        set_progress(total ? (int)((uint64_t)done * 100 / total) : 100);
    }
    free(buf);
    if (close(fd) != 0 && !fail_msg) {
        fail_msg = "write failed";
    }
    if (fail_msg) {
        unlink(vfs);
        ESP_LOGW(TAG, "Upload %s failed: %s", vfs, fail_msg);
        set_activity("Upload of %.40s failed", name, -1);
        return send_error(req, "500 Internal Server Error", fail_msg);
    }

    long mtime = query_long(req, "mtime", 0);
    if (mtime > 315532800) { // FAT can't store dates before 1980
        struct utimbuf tb = { .actime = mtime, .modtime = mtime };
        utime(vfs, &tb);
    }
    set_activity("Received %.50s", name, -1);
    s_changes++;
    return send_ok(req);
}

static esp_err_t mkdir_post(httpd_req_t *req)
{
    char vfs[VFS_PATH_LEN];
    if (!query_path(req, "path", vfs, sizeof(vfs))) {
        return send_error(req, "400 Bad Request", "bad path");
    }
    if (mkdir(vfs, 0775) != 0) {
        return send_errno(req, "Can't create folder");
    }
    set_activity("Created folder %.45s", base_name(vfs), -1);
    s_changes++;
    return send_ok(req);
}

// Remove a file, or a folder and everything in it. `path` is a buffer of VFS_PATH_LEN + 260 that
// gets extended in place while descending.
static int remove_tree(char *path, size_t cap, int depth)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        return unlink(path);
    }
    if (depth > 16) {
        errno = ENAMETOOLONG;
        return -1;
    }
    size_t len = strlen(path);
    for (;;) {
        // reopen after each removal: deleting while iterating a FAT directory skips entries
        DIR *dir = opendir(path);
        if (!dir) {
            return -1;
        }
        struct dirent *de;
        bool found = false;
        while ((de = readdir(dir)) != NULL) {
            if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) {
                found = true;
                break;
            }
        }
        if (found && len + 1 + strlen(de->d_name) + 1 > cap) {
            closedir(dir);
            errno = ENAMETOOLONG;
            return -1;
        }
        if (found) {
            snprintf(path + len, cap - len, "/%s", de->d_name);
        }
        closedir(dir);
        if (!found) {
            break;
        }
        int r = remove_tree(path, cap, depth + 1);
        path[len] = '\0';
        if (r != 0) {
            return -1;
        }
    }
    return rmdir(path);
}

static esp_err_t delete_post(httpd_req_t *req)
{
    char vfs[VFS_PATH_LEN + 260];
    if (!query_path(req, "path", vfs, VFS_PATH_LEN) || strcmp(vfs, SDCARD_MOUNT_POINT) == 0) {
        return send_error(req, "400 Bad Request", "bad path");
    }
    char name[64];
    strlcpy(name, base_name(vfs), sizeof(name));
    if (remove_tree(vfs, sizeof(vfs), 0) != 0) {
        return send_errno(req, "Can't delete");
    }
    set_activity("Deleted %.55s", name, -1);
    s_changes++;
    return send_ok(req);
}

static esp_err_t rename_post(httpd_req_t *req)
{
    char from[VFS_PATH_LEN], to[VFS_PATH_LEN];
    if (!query_path(req, "from", from, sizeof(from)) || !query_path(req, "to", to, sizeof(to)) ||
        strcmp(from, SDCARD_MOUNT_POINT) == 0 || strcmp(to, SDCARD_MOUNT_POINT) == 0) {
        return send_error(req, "400 Bad Request", "bad path");
    }
    struct stat st;
    if (stat(to, &st) == 0) {
        return send_error(req, "409 Conflict", "something with that name already exists");
    }
    if (rename(from, to) != 0) {
        return send_errno(req, "Can't rename");
    }
    set_activity("Renamed to %.50s", base_name(to), -1);
    s_changes++;
    return send_ok(req);
}

static esp_err_t flash_post(httpd_req_t *req)
{
    char vfs[VFS_PATH_LEN];
    struct stat st;
    if (!query_path(req, "path", vfs, sizeof(vfs))) {
        return send_error(req, "400 Bad Request", "bad path");
    }
    if (stat(vfs, &st) != 0 || S_ISDIR(st.st_mode)) {
        return send_error(req, "404 Not Found", "no such file");
    }
    LOCKED({
        strlcpy(s_flash_path, vfs, sizeof(s_flash_path));
        s_flash_pending = true;
    });
    ESP_LOGI(TAG, "Flash requested: %s", vfs);
    return send_ok(req);
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    char body[160];
    if (req->content_len == 0 || req->content_len >= sizeof(body)) {
        return send_error(req, "400 Bad Request", "send \"ssid\\npassword\"");
    }
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) {
            return ESP_FAIL;
        }
        got += n;
    }
    body[got] = '\0';
    char *nl = strchr(body, '\n');
    size_t ssid_len = nl ? (size_t)(nl - body) : strlen(body);
    size_t pass_len = nl ? strlen(nl + 1) : 0;
    if (ssid_len == 0 || ssid_len > 32 || pass_len > 64 || (pass_len > 0 && pass_len < 8)) {
        return send_error(req, "400 Bad Request",
                          "The network name must be 1-32 characters, and the password 8-64 (or empty)");
    }
    body[ssid_len] = '\0';
    if (web_wifi_save(body, nl ? nl + 1 : "") != ESP_OK) {
        return send_error(req, "500 Internal Server Error", "could not save the settings");
    }
    set_activity("%s", "Saved Wi-Fi settings", -1);
    return send_ok(req);
}

// ---- Wi-Fi ------------------------------------------------------------------------------------
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        ESP_LOGW(TAG, "Disconnected (reason %d)", d->reason);
        s_last_reason = d->reason;
        LOCKED(s_status.ip[0] = '\0');
        if (s_want_sta && !s_stopping) {
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        LOCKED(snprintf(s_status.ip, sizeof(s_status.ip), IPSTR, IP2STR(&e->ip_info.ip)));
        ESP_LOGI(TAG, "Got IP " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_events, BIT_GOT_IP);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        set_activity("%s", "A device joined the access point", -1);
    }
}

static esp_err_t wifi_init_once(void)
{
    if (s_wifi_ready) {
        return ESP_OK;
    }
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta_netif, WEB_HOSTNAME);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if ((err = esp_wifi_init(&cfg)) != ESP_OK) {
        return err;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    s_wifi_ready = true;
    return ESP_OK;
}

// ---- saved network (NVS) ----------------------------------------------------------------------
bool web_wifi_load(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    ssid[0] = pass[0] = '\0';
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t n = ssid_len;
    bool ok = nvs_get_str(h, "wifi_ssid", ssid, &n) == ESP_OK && ssid[0];
    n = pass_len;
    if (ok && nvs_get_str(h, "wifi_pass", pass, &n) != ESP_OK) {
        pass[0] = '\0';
    }
    nvs_close(h);
    return ok;
}

esp_err_t web_wifi_save(const char *ssid, const char *pass)
{
    if (strlen(ssid) > 32 || strlen(pass) > 64 || (pass[0] && strlen(pass) < 8)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    if (ssid[0]) {
        err = nvs_set_str(h, "wifi_ssid", ssid);
        if (err == ESP_OK) {
            err = nvs_set_str(h, "wifi_pass", pass);
        }
    } else { // forget the network
        nvs_erase_key(h, "wifi_ssid");
        nvs_erase_key(h, "wifi_pass");
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static int cmp_rssi(const void *a, const void *b)
{
    return ((const web_ap_t *)b)->rssi - ((const web_ap_t *)a)->rssi;
}

int web_wifi_scan(web_ap_t *out, int max)
{
    if (s_status.state != WEB_OFF || wifi_init_once() != ESP_OK) {
        return -1;
    }
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK || esp_wifi_start() != ESP_OK) {
        return -1;
    }
    int count = -1;
    uint16_t num = 0;
    wifi_ap_record_t *recs = NULL;
    if (esp_wifi_scan_start(NULL, true) == ESP_OK && esp_wifi_scan_get_ap_num(&num) == ESP_OK) {
        recs = calloc(num ? num : 1, sizeof(*recs));
        if (recs && esp_wifi_scan_get_ap_records(&num, recs) == ESP_OK) {
            count = 0;
            for (int i = 0; i < num && count < max; i++) {
                const char *name = (const char *)recs[i].ssid;
                bool dup = !name[0];
                for (int j = 0; j < count && !dup; j++) {
                    dup = strcmp(out[j].ssid, name) == 0; // keep the strongest of each name
                }
                if (dup) {
                    for (int j = 0; j < count; j++) {
                        if (strcmp(out[j].ssid, name) == 0 && recs[i].rssi > out[j].rssi) {
                            out[j].rssi = recs[i].rssi;
                        }
                    }
                    continue;
                }
                strlcpy(out[count].ssid, name, sizeof(out[count].ssid));
                out[count].rssi = recs[i].rssi;
                out[count].secure = recs[i].authmode != WIFI_AUTH_OPEN;
                count++;
            }
            qsort(out, count, sizeof(*out), cmp_rssi);
        }
    }
    free(recs);
    esp_wifi_stop();
    ESP_LOGI(TAG, "Scan found %d networks", count);
    return count;
}

static bool try_station(const char *ssid, const char *pass)
{
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    LOCKED({
        s_status.state = WEB_CONNECTING;
        s_status.ap_mode = false;
        strlcpy(s_status.ssid, ssid, sizeof(s_status.ssid));
    });
    ESP_LOGI(TAG, "Joining \"%s\"", ssid);
    xEventGroupClearBits(s_events, BIT_GOT_IP);
    s_last_reason = 0;
    s_want_sta = true;
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK || esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK ||
        esp_wifi_start() != ESP_OK) {
        s_want_sta = false;
        return false;
    }
    esp_wifi_connect();
    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_GOT_IP | BIT_STOP, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
    if (bits & BIT_GOT_IP) {
        return true;
    }
    ESP_LOGW(TAG, "Could not join \"%s\"", ssid);
    switch (s_last_reason) {
    case WIFI_REASON_NO_AP_FOUND:
        set_activity("%.34s not found (2.4 GHz only)", ssid, -1);
        break;
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
        set_activity("Wrong password for %.36s?", ssid, -1);
        break;
    default:
        set_activity("Could not join %.40s", ssid, -1);
        break;
    }
    s_want_sta = false;
    esp_wifi_disconnect();
    esp_wifi_stop();
    return false;
}

// The AP password is random, made once and kept in NVS, so a phone that saved the network can
// rejoin next time.
static void ap_password(char *out, size_t len)
{
    nvs_handle_t h;
    size_t n = len;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        strlcpy(out, "picocalc", len);
        return;
    }
    if (nvs_get_str(h, "ap_pass", out, &n) != ESP_OK || strlen(out) < 8) {
        static const char chars[] = "abcdefghjkmnpqrstuvwxyz23456789"; // no 0/o, 1/l/i
        int i;
        for (i = 0; i < 10 && i + 1 < (int)len; i++) {
            out[i] = chars[esp_random() % (sizeof(chars) - 1)];
        }
        out[i] = '\0';
        nvs_set_str(h, "ap_pass", out);
        nvs_commit(h);
    }
    nvs_close(h);
}

static esp_err_t start_access_point(void)
{
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t mac[6] = { 0 };
    esp_wifi_get_mac(WIFI_IF_AP, mac); // the C6's radio MAC (the P4 has none of its own)
    wifi_config_t wc = { 0 };
    char pass[16];
    ap_password(pass, sizeof(pass));
    int n = snprintf((char *)wc.ap.ssid, sizeof(wc.ap.ssid), AP_SSID_PREFIX "-%02X%02X", mac[4], mac[5]);
    wc.ap.ssid_len = n;
    strlcpy((char *)wc.ap.password, pass, sizeof(wc.ap.password));
    wc.ap.channel = AP_CHANNEL;
    wc.ap.max_connection = 4;
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;

    if ((err = esp_wifi_set_config(WIFI_IF_AP, &wc)) != ESP_OK || (err = esp_wifi_start()) != ESP_OK) {
        return err;
    }
    esp_netif_ip_info_t ip = { 0 };
    esp_netif_get_ip_info(s_ap_netif, &ip);
    LOCKED({
        s_status.ap_mode = true;
        strlcpy(s_status.ssid, (char *)wc.ap.ssid, sizeof(s_status.ssid));
        strlcpy(s_status.password, pass, sizeof(s_status.password));
        snprintf(s_status.ip, sizeof(s_status.ip), IPSTR, IP2STR(&ip.ip));
    });
    ESP_LOGI(TAG, "Access point \"%s\" password %s, " IPSTR, (char *)wc.ap.ssid, pass, IP2STR(&ip.ip));
    return ESP_OK;
}

static esp_err_t start_http(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 12;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    static const httpd_uri_t uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = index_get },
        { .uri = "/api/list", .method = HTTP_GET, .handler = list_get },
        { .uri = "/api/info", .method = HTTP_GET, .handler = info_get },
        { .uri = "/api/file", .method = HTTP_GET, .handler = file_get },
        { .uri = "/api/upload", .method = HTTP_POST, .handler = upload_post },
        { .uri = "/api/mkdir", .method = HTTP_POST, .handler = mkdir_post },
        { .uri = "/api/delete", .method = HTTP_POST, .handler = delete_post },
        { .uri = "/api/rename", .method = HTTP_POST, .handler = rename_post },
        { .uri = "/api/flash", .method = HTTP_POST, .handler = flash_post },
        { .uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_post },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_httpd, &uris[i]);
    }

    if (mdns_init() == ESP_OK) {
        s_mdns = true;
        mdns_hostname_set(WEB_HOSTNAME);
        mdns_instance_name_set("PicoMultiLoader");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
    return ESP_OK;
}

static void web_task(void *arg)
{
    esp_err_t err = wifi_init_once();
    if (err != ESP_OK) {
        fail("The ESP32-C6 Wi-Fi chip did not start", err);
        goto done;
    }

    char ssid[33], pass[65];
    bool joined = false;
    if (web_wifi_load(ssid, sizeof(ssid), pass, sizeof(pass))) {
        joined = try_station(ssid, pass);
    } else {
        ESP_LOGI(TAG, "No Wi-Fi network set up (F1), starting an access point");
    }
    if (s_stopping) {
        goto done;
    }
    if (!joined && (err = start_access_point()) != ESP_OK) {
        fail("Could not start the access point", err);
        goto done;
    }
    if ((err = start_http()) != ESP_OK) {
        fail("Could not start the web server", err);
        goto done;
    }
    if (joined || !s_status.activity[0]) {
        set_activity("%s", "Waiting for a browser", -1); // else keep why joining failed
    }
    set_state(WEB_RUNNING);

done:
    xEventGroupSetBits(s_events, BIT_DONE);
    vTaskDelete(NULL);
}

esp_err_t webserver_start(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        s_events = xEventGroupCreate();
        if (!s_lock || !s_events) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_status.state != WEB_OFF) {
        return ESP_ERR_INVALID_STATE;
    }
    s_stopping = false;
    s_flash_pending = false;
    xEventGroupClearBits(s_events, BIT_GOT_IP | BIT_STOP | BIT_DONE);
    LOCKED({
        memset(&s_status, 0, sizeof(s_status));
        s_status.state = WEB_STARTING;
        s_status.progress = -1;
    });
    if (xTaskCreate(web_task, "web_start", 6144, NULL, 5, NULL) != pdPASS) {
        set_state(WEB_OFF);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void webserver_stop(void)
{
    if (!s_lock || s_status.state == WEB_OFF) {
        return;
    }
    s_stopping = true;
    xEventGroupSetBits(s_events, BIT_STOP);
    xEventGroupWaitBits(s_events, BIT_DONE, pdFALSE, pdTRUE, portMAX_DELAY);
    if (s_mdns) {
        mdns_free();
        s_mdns = false;
    }
    if (s_httpd) {
        httpd_stop(s_httpd); // waits for a running handler; they check s_stopping
        s_httpd = NULL;
    }
    s_want_sta = false;
    if (s_wifi_ready) {
        esp_wifi_disconnect();
        esp_wifi_stop();
    }
    set_state(WEB_OFF);
    ESP_LOGI(TAG, "Web server stopped");
}
