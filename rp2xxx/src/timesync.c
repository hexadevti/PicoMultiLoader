// timesync.c - set the clock from the web while Wi-Fi is on as a station.
//
// One plain HTTP request to ip-api.com does both jobs: the reply's Date header is the time (UTC,
// to the second) and its JSON body is the time zone of our public IP, daylight saving included.
#include "timesync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "pico/stdlib.h"
#include "lwip/dns.h"
#include "lwip/tcp.h"
#include "clock.h"

#define SYNC_HOST    "ip-api.com"
#define SYNC_REQUEST "GET /json/?fields=status,offset,timezone HTTP/1.1\r\n" \
                     "Host: " SYNC_HOST "\r\nUser-Agent: PicoMultiLoader\r\nConnection: close\r\n\r\n"
#define SYNC_EVERY_US   (6ull * 3600 * 1000000) // re-sync every 6 hours
#define RETRY_US        (60ull * 1000000)       // after a failure
#define TIMEOUT_US      (15ull * 1000000)
#define REPLY_MAX       1024

typedef enum { TS_IDLE, TS_DNS, TS_CONNECT, TS_WAIT } ts_state_t;

static ts_state_t s_state;
static uint64_t s_next_us;     // when to sync next
static uint64_t s_deadline_us; // give up on the current attempt
static struct tcp_pcb *s_pcb;
static char *s_reply;
static int s_reply_len;
static bool s_done;            // reply complete (the server closed)
static bool s_dns_done;
static ip_addr_t s_addr;
static bool s_dns_ok;

static void drop(void)
{
    if (s_pcb) {
        tcp_arg(s_pcb, NULL);
        tcp_recv(s_pcb, NULL);
        tcp_err(s_pcb, NULL);
        tcp_sent(s_pcb, NULL);
        if (tcp_close(s_pcb) != ERR_OK) {
            tcp_abort(s_pcb);
        }
        s_pcb = NULL;
    }
    free(s_reply);
    s_reply = NULL;
    s_reply_len = 0;
    s_state = TS_IDLE;
}

static void failed(const char *why)
{
    printf("time: %s\n", why);
    drop();
    s_next_us = time_us_64() + RETRY_US;
}

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
        // by hand: sscanf would pull in ~35 KB of newlib (wide-char tables, strtod) and RAM is tight
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
        printf("time: no time zone in the reply\n");
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

static void finish(void)
{
    s_reply[s_reply_len] = '\0';
    char *body = strstr(s_reply, "\r\n\r\n");
    int64_t utc;
    if (strncmp(s_reply, "HTTP/1.", 7) != 0 || !body) {
        failed("bad reply");
        return;
    }
    *body = '\0';
    if (!parse_date(s_reply, &utc)) {
        failed("no Date in the reply");
        return;
    }
    clock_set_utc(utc);
    parse_body(body + 4);
    clock_tm_t tm;
    clock_local(&tm);
    printf("time: %04d-%02d-%02d %02d:%02d:%02d %s\n", tm.year, tm.month, tm.day, tm.hour, tm.min, tm.sec,
           clock_zone_name());
    drop();
    s_next_us = time_us_64() + SYNC_EVERY_US;
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    if (!p) {
        s_done = true; // the server closed: the reply is complete
        return ERR_OK;
    }
    for (struct pbuf *q = p; q; q = q->next) {
        int n = q->len;
        if (n > REPLY_MAX - s_reply_len) {
            n = REPLY_MAX - s_reply_len;
        }
        memcpy(s_reply + s_reply_len, q->payload, (size_t)n);
        s_reply_len += n;
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    if (s_reply_len >= REPLY_MAX) {
        s_done = true; // more than we need: parse what we have
    }
    return ERR_OK;
}

static void on_err(void *arg, err_t err)
{
    s_pcb = NULL; // already freed by lwIP
    if (s_state != TS_IDLE && !s_done) {
        failed("connection error");
    }
}

static err_t on_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
    if (err != ERR_OK || tcp_write(pcb, SYNC_REQUEST, sizeof(SYNC_REQUEST) - 1, 0) != ERR_OK) {
        return ERR_OK; // poll() times out and cleans up
    }
    tcp_output(pcb);
    s_state = TS_WAIT;
    return ERR_OK;
}

static void on_dns(const char *name, const ip_addr_t *addr, void *arg)
{
    s_dns_ok = addr != NULL;
    if (addr) {
        s_addr = *addr;
    }
    s_dns_done = true;
}

static void start_connect(void)
{
    s_reply = malloc(REPLY_MAX + 1);
    s_pcb = s_reply ? tcp_new_ip_type(IPADDR_TYPE_V4) : NULL;
    if (!s_pcb) {
        failed("out of memory");
        return;
    }
    s_reply_len = 0;
    s_done = false;
    tcp_recv(s_pcb, on_recv);
    tcp_err(s_pcb, on_err);
    s_state = TS_CONNECT;
    if (tcp_connect(s_pcb, &s_addr, 80, on_connected) != ERR_OK) {
        failed("can't connect");
    }
}

void timesync_poll(void)
{
    uint64_t now = time_us_64();
    switch (s_state) {
    case TS_IDLE:
        if (now >= s_next_us) {
            s_deadline_us = now + TIMEOUT_US;
            s_dns_done = false;
            err_t e = dns_gethostbyname(SYNC_HOST, &s_addr, on_dns, NULL);
            if (e == ERR_OK) {
                start_connect();
            } else if (e == ERR_INPROGRESS) {
                s_state = TS_DNS;
            } else {
                failed("DNS error");
            }
        }
        break;
    case TS_DNS:
        if (s_dns_done) {
            if (s_dns_ok) {
                start_connect();
            } else {
                failed("can't find " SYNC_HOST);
            }
        } else if (now >= s_deadline_us) {
            failed("DNS timed out");
        }
        break;
    case TS_CONNECT:
    case TS_WAIT:
        if (s_done) {
            finish();
        } else if (now >= s_deadline_us) {
            failed("timed out");
        }
        break;
    }
}

void timesync_stop(void)
{
    drop();
    s_dns_done = false;
    s_next_us = 0; // sync again as soon as Wi-Fi is back
}
