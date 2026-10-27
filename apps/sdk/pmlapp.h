// pmlapp.h - the small library the bundled apps (apps/) share: start-up, keys, the screen frame
// in the loader's look, dialogs, a file picker, per-app settings and "exit to the menu".
//
// It reuses the loader's drivers: the panel bus and SD card from rp2xxx/src, the keyboard,
// themes and settings from common/pml. The framebuffer is 8 bits per pixel (lcd8.c): entries
// 0..15 are the theme's palette roles (look.h), 16..255 are the app's own.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "keyboard.h"
#include "lcd.h"
#include "look.h"
#include "sdcard.h"

// The screen frame: a title bar at the top, a key bar at the bottom, the app's body between.
#define APP_HEADER_H 18
#define APP_FOOTER_H 14
#define APP_TOP      APP_HEADER_H
#define APP_BOTTOM   (LCD_H - APP_FOOTER_H)
#define APP_BODY_H   (APP_BOTTOM - APP_TOP)

#define KEY_CTRL(c) ((c) & 0x1F)

// APP_DIR (the app's folder on the card, e.g. "/TextEditor") and APP_NAME come from apps/CMakeLists.txt.

/** Console, SD card, LCD, keyboard and the user's theme (the loader's "theme" setting). */
void app_init(void);

/** True if the SD card is mounted. */
bool app_sd_ok(void);

/** A function the key loops call while they wait (e.g. to keep audio playing); NULL for none. */
void app_set_idle(void (*fn)(void));

/** Run the idle function, cycle the theme's colours, read the battery now and then. */
void app_idle(void);

/** The next key from the keyboard or the serial console, or KEY_NONE. Calls app_idle(). */
int app_poll_key(void);

/** Wait for a key. */
int app_wait_key(void);

/** One framebuffer row (LCD_W bytes, one palette index per pixel). */
uint8_t *app_fb_row(int y);

/** Battery percent (or -1), as last read. */
int app_battery(bool *charging);

// ---- the frame ----------------------------------------------------------------------------------

/** The title bar: `title` on the left, `right` (may be NULL) before the battery. */
void app_header(const char *title, const char *right);

/** The key bar, e.g. "F1 Help  F2 Save  Esc Quit". */
void app_footer(const char *keys);

/** Clear the body (between the bars) to C_BG. */
void app_clear_body(void);

/** Draw at most `max_chars` characters of `s`; a longer string ends in "~". Returns the x after. */
int app_text_clip(int x, int y, const char *s, int max_chars, lcd_color_t color, lcd_font_t font);

// ---- dialogs (they draw over the screen and flush; redraw it afterwards) --------------------------

/** A message; any key closes it. */
void app_message(const char *title, const char *text);

/** Yes / no: Enter or Y is yes, Esc or N is no. */
bool app_confirm(const char *title, const char *text);

/** Yes / no / cancel: 1, 0 or -1. */
int app_ask(const char *title, const char *text);

/** Edit one line of text in `buf` (it starts with its current contents). False if cancelled. */
bool app_input(const char *title, const char *prompt, char *buf, size_t len);

/**
 * Pick a file whose extension is in `exts` (e.g. ".wav" or ".txt,.md,.bas", empty for any file).
 * `path` holds a folder or a file to start at, and receives the chosen file. False if cancelled.
 */
bool app_pick_file(const char *title, const char *exts, char *path, size_t len);

/** A help screen: lines of text, "" for a gap. Up / Down scroll, any other key closes it. */
void app_help(const char *title, const char *const *lines, int count);

/** A short note in the key bar that stays until the next app_footer(). */
void app_toast(const char *text);

// ---- files --------------------------------------------------------------------------------------

/** True if `name` ends in one of the comma-separated extensions (case-insensitive). */
bool app_has_ext(const char *name, const char *exts);

/** The folder part of `path` ("/a/b.txt" -> "/a", "/b.txt" -> ""). */
void app_dirname(const char *path, char *out, size_t len);

/** The file name part of `path`. */
const char *app_basename(const char *path);

/** A setting kept in APP_DIR/settings.ini; false if unset. */
bool app_setting_get(const char *key, char *buf, size_t len);

/** Store a setting (does nothing if the app's folder isn't on the card). */
void app_setting_set(const char *key, const char *value);

/** Go back to the loader's menu, leaving `message` (may be NULL) for it to show once. */
void __attribute__((noreturn)) app_exit(const char *message);
