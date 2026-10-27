// timesync.c - set the clock from the web while Wi-Fi is on as a station.
//
// One plain HTTP request to ip-api.com does both jobs: the reply's Date header is the time (UTC,
// to the second) and its JSON body is the time zone of our public IP, daylight saving included.
// A short-lived task makes the request (sockets block); timesync_poll() applies the result on the
// main thread, which owns the clock and the settings file.
#include "timesync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "clock.h"
#include "plat.h"

static const char *TAG = "time";

#define SYNC_HOST    "ip-api.com"
#define SYNC_REQUEST "GET /json/?fields=status,offset,timezone HTTP/1.1\r\n" \
                     "Host: " SYNC_HOST "\r\nUser-Agent: PicoMultiLoader\r\nConnection: close\r\n\r\n"
#define SYNC_EVERY_US   (6ull * 3600 * 1000000) // re-sync every 6 hours
#define RETRY_US        (60ull * 1000000)       // after a failure
#define TIMEOUT_S       15
#define REPLY_MAX       1024

typedef enum { TS_IDLE, TS_BUSY, TS_DONE } ts_state_t;

static volatile ts_state_t s_state;
static volatile bool s_cancel;  // Wi-Fi went down: drop the reply
static uint64_t s_next_us;      // when to sync next
static char s_reply[REPLY_MAX + 1];
static int s_reply_len;         // < 0: the request failed

// "Date: Thu, 23 Oct 2026 12:34:56 GMT"
static bool parse_date(const char *headers, int64_t *utc)
{
    static const char MONTHS[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    for (const char *p = headers; p; p = strchr(p, '\n')) {
        if (*p == '\n') {
            p++;
        }
        if (strncasecmp(p, "Date:", 5) != 0) {
            continue;
        }
        const char *s = strchr(p, ',');
        char *e;
        if (!s) {
            return false;
        }
        int d = (int)strtol(s + 1, &e, 10);
        while (*e == ' ') {
            e++;
        }
        char mon[4] = {e[0], e[0] ? e[1] : 0, e[0] && e[1] ? e[2] : 0, 0};
        int y = (int)strtol(e + 3, &e, 10);
        int hh = (int)strtol(e, &e, 10);
        int mm = *e == ':' ? (int)strtol(e + 1, &e, 10) : -1;
        int ss = *e == ':' ? (int)strtol(e + 1, &e, 10) : -1;
        const char *m = strlen(mon) == 3 ? strstr(MONTHS, mon) : NULL;
        if (!m || (m - MONTHS) % 3 || y < 2024 || d < 1 || d > 31 || mm < 0 || ss < 0) {
            return false;
        }
        *utc = clock_days_from_civil(y, (unsigned)((m - MONTHS) / 3 + 1), (unsigned)d) * 86400 +
               hh * 3600 + mm * 60 + ss;
        return true;
    }
    return false;
}

// {"status":"success","timezone":"Europe/Lisbon","offset":3600}
static void parse_body(const char *body)
{
    const char *off = strstr(body, "\"offset\":");
    const char *tz = strstr(body, "\"timezone\":\"");
    if (!strstr(body, "\"success\"") || !off) {
        ESP_LOGW(TAG, "No time zone in the reply");
        return;
    }
    char name[40] = "";
    if (tz) {
        tz += 12;
        size_t n = strcspn(tz, "\"");
        if (n >= sizeof(name)) {
            n = sizeof(name) - 1;
        }
        memcpy(name, tz, n);
        name[n] = '\0';
    }
    clock_set_zone((int)strtol(off + 9, NULL, 10), name);
}

static bool finish(void)
{
    s_reply[s_reply_len] = '\0';
    char *body = strstr(s_reply, "\r\n\r\n");
    int64_t utc;
    if (strncmp(s_reply, "HTTP/1.", 7) != 0 || !body) {
        ESP_LOGW(TAG, "Bad reply");
        return false;
    }
    *body = '\0';
    if (!parse_date(s_reply, &utc)) {
        ESP_LOGW(TAG, "No Date in the reply");
        return false;
    }
    clock_set_utc(utc);
    parse_body(body + 4);
    clock_tm_t tm;
    clock_local(&tm);
    ESP_LOGI(TAG, "%04d-%02d-%02d %02d:%02d:%02d %s", tm.year, tm.month, tm.day, tm.hour, tm.min, tm.sec,
             clock_zone_name());
    return true;
}

static int fetch(void)
{
    const struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    if (getaddrinfo(SYNC_HOST, "80", &hints, &res) != 0 || !res) {
        ESP_LOGW(TAG, "Can't find " SYNC_HOST);
        return -1;
    }
    int len = -1;
    int fd = socket(res->ai_family, res->ai_socktype, 0);
    if (fd >= 0) {
        struct timeval tv = { .tv_sec = TIMEOUT_S };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(fd, res->ai_addr, res->ai_addrlen) == 0 &&
            send(fd, SYNC_REQUEST, sizeof(SYNC_REQUEST) - 1, 0) == (int)sizeof(SYNC_REQUEST) - 1) {
            len = 0;
            int n;
            while (len < REPLY_MAX && !s_cancel && (n = recv(fd, s_reply + len, REPLY_MAX - len, 0)) > 0) {
                len += n; // until the server closes, or we have more than we need
            }
        } else {
            ESP_LOGW(TAG, "Can't connect");
        }
        close(fd);
    }
    freeaddrinfo(res);
    return len;
}

static void sync_task(void *arg)
{
    s_reply_len = fetch();
    s_state = TS_DONE;
    vTaskDelete(NULL);
}

void timesync_poll(void)
{
    uint64_t now = time_us_64();
    switch (s_state) {
    case TS_IDLE:
        if (now >= s_next_us) {
            s_cancel = false;
            s_state = TS_BUSY;
            if (xTaskCreate(sync_task, "timesync", 4096, NULL, 4, NULL) != pdPASS) {
                s_state = TS_IDLE;
                s_next_us = now + RETRY_US;
            }
        }
        break;
    case TS_BUSY:
        break;
    case TS_DONE:
        s_state = TS_IDLE;
        if (s_cancel) {
            break; // stopped meanwhile: s_next_us is 0, so sync again when Wi-Fi is back
        }
        s_next_us = now + (s_reply_len > 0 && finish() ? SYNC_EVERY_US : RETRY_US);
        break;
    }
}

void timesync_stop(void)
{
    s_cancel = true; // a request in flight ends by itself (timeouts) and is ignored
    s_next_us = 0;   // sync again as soon as Wi-Fi is back
}
