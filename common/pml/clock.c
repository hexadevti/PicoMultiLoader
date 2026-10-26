// clock.c - the wall clock: a UTC base set from the web plus the time since, and the time zone
#include "clock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "config.h"
#include "ff.h"
#if PML_P4
#include <sys/time.h>
#include <time.h>
#endif

static bool s_known;
static int64_t s_base_utc;   // UTC at s_base_us
static uint64_t s_base_us;
static int s_auto_offset;    // from the web
static char s_auto_name[40];
static bool s_manual;        // "tz" set by hand
static int s_manual_offset;
static char s_manual_name[16];

#if PML_P4
// ESP-IDF's FatFs stamps files from time() and localtime_r() (its own get_fattime()): keep the
// system clock and TZ in step with this one.
static void sync_system(void)
{
    if (s_known) {
        struct timeval tv = {.tv_sec = (time_t)clock_utc()};
        settimeofday(&tv, NULL);
    }
    int off = clock_offset(), a = off < 0 ? -off : off;
    char tz[16];
    snprintf(tz, sizeof(tz), "UTC%c%02d:%02d", off < 0 ? '+' : '-', a / 3600, a / 60 % 60); // POSIX: west is +
    setenv("TZ", tz, 1);
    tzset();
}
#else
static void sync_system(void)
{
}
#endif

void clock_set_utc(int64_t utc)
{
    s_base_utc = utc;
    s_base_us = time_us_64();
    s_known = true;
    sync_system();
}

void clock_set_zone(int offset_s, const char *name)
{
    s_auto_offset = offset_s;
    strlcpy(s_auto_name, name ? name : "", sizeof(s_auto_name));
    sync_system();
}

bool clock_known(void)
{
    return s_known;
}

int64_t clock_utc(void)
{
    return s_known ? s_base_utc + (int64_t)((time_us_64() - s_base_us) / 1000000) : 0;
}

int clock_offset(void)
{
    return s_manual ? s_manual_offset : s_auto_offset;
}

const char *clock_zone_name(void)
{
    if (s_manual) {
        return s_manual_name;
    }
    return s_auto_name[0] ? s_auto_name : "UTC";
}

int64_t clock_days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + doe - 719468;
}

void clock_civil(int64_t t, clock_tm_t *out)
{
    int64_t days = t >= 0 ? t / 86400 : (t - 86399) / 86400;
    int secs = (int)(t - days * 86400);
    int64_t z = days + 719468;
    int era = (int)((z >= 0 ? z : z - 146096) / 146097);
    unsigned doe = (unsigned)(z - (int64_t)era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    out->year = (int)yoe + era * 400 + (m <= 2);
    out->month = (int)m;
    out->day = (int)(doy - (153 * mp + 2) / 5 + 1);
    out->hour = secs / 3600;
    out->min = secs / 60 % 60;
    out->sec = secs % 60;
    out->wday = (int)((days % 7 + 11) % 7); // 1970-01-01 was a Thursday
}

bool clock_local(clock_tm_t *out)
{
    if (!s_known) {
        return false;
    }
    clock_civil(clock_utc() + clock_offset(), out);
    return true;
}

void clock_date_text(const clock_tm_t *tm, char *out, size_t len)
{
    static const char DAYS[] = "SunMonTueWedThuFriSat";
    static const char MONTHS[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    snprintf(out, len, "%.3s %d %.3s %d", DAYS + 3 * tm->wday, tm->day, MONTHS + 3 * (tm->month - 1), tm->year);
}

// "+hh:mm", "-hh", "+h:mm" -> seconds east of UTC
static bool parse_offset(const char *s, int *out)
{
    if (*s != '+' && *s != '-') {
        return false;
    }
    int sign = *s++ == '-' ? -1 : 1;
    char *end;
    long h = strtol(s, &end, 10);
    long m = 0;
    if (end == s || h > 14) {
        return false;
    }
    if (*end == ':') {
        s = end + 1;
        m = strtol(s, &end, 10);
        if (end == s || m > 59) {
            return false;
        }
    }
    if (*end) {
        return false;
    }
    *out = sign * (int)(h * 3600 + m * 60);
    return true;
}

static void apply(const char *value)
{
    int off;
    s_manual = parse_offset(value, &off);
    if (s_manual) {
        s_manual_offset = off;
        int a = off < 0 ? -off : off;
        snprintf(s_manual_name, sizeof(s_manual_name), "UTC%c%02d:%02d", off < 0 ? '-' : '+', a / 3600, a / 60 % 60);
    }
    sync_system();
}

void clock_load_setting(void)
{
    char v[16];
    apply(config_get("tz", v, sizeof(v)) ? v : "auto");
}

bool clock_set_setting(const char *value)
{
    int off;
    bool is_auto = strcmp(value, "auto") == 0;
    if (!is_auto && !parse_offset(value, &off)) {
        return false;
    }
    config_set("tz", is_auto ? "" : value);
    apply(value);
    return true;
}

#if !PML_P4
// FatFs stamps the files it writes with this (FF_FS_NORTC = 0).
DWORD get_fattime(void)
{
    clock_tm_t tm;
    if (!clock_local(&tm) || tm.year < 1980 || tm.year > 2107) {
        return (DWORD)(2025 - 1980) << 25 | 1u << 21 | 1u << 16;
    }
    return (DWORD)(tm.year - 1980) << 25 | (DWORD)tm.month << 21 | (DWORD)tm.day << 16 |
           (DWORD)tm.hour << 11 | (DWORD)tm.min << 5 | (DWORD)(tm.sec / 2);
}
#endif
