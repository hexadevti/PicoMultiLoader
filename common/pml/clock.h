// clock.h - the wall clock. There is no RTC: the time is set from the web while Wi-Fi is on
// (timesync.c) and counted from time_us_64() after that, so it is lost on power-off or app launch.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int year, month, day; // 1-12, 1-31
    int hour, min, sec;
    int wday;             // 0 = Sunday
} clock_tm_t;

/** Set the time (seconds since 1970, UTC). */
void clock_set_utc(int64_t utc);

/** Set the time zone found from the web: its offset from UTC and its name ("Europe/Lisbon"). */
void clock_set_zone(int offset_s, const char *name);

/** True once the time has been set. */
bool clock_known(void);

/** Seconds since 1970, UTC (0 if the time isn't known). */
int64_t clock_utc(void);

/** The local time; false if it isn't known. */
bool clock_local(clock_tm_t *out);

/** The time zone in use: "Europe/Lisbon", or "UTC+05:30" when set by hand. */
const char *clock_zone_name(void);

/** Seconds east of UTC now in use. */
int clock_offset(void);

/** Read the "tz" setting: "auto" (default, from the web) or "+hh:mm" / "-hh:mm". */
void clock_load_setting(void);

/** Change the "tz" setting. False if `value` isn't "auto" or a valid offset. */
bool clock_set_setting(const char *value);

/** Days since 1970-01-01 of a civil date, and back. */
int64_t clock_days_from_civil(int y, unsigned m, unsigned d);
void clock_civil(int64_t t, clock_tm_t *out);

/** "Thu 23 Oct 2026" */
void clock_date_text(const clock_tm_t *tm, char *out, size_t len);
