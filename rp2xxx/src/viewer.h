// viewer.h - the file viewers opened from Files: a paged text reader (.txt, .md) and a BMP viewer,
// and screenshots of the LCD saved as BMP files. ui.c owns the screens; this draws their content.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    VIEW_NONE,
    VIEW_TEXT,
    VIEW_MARKDOWN,
    VIEW_IMAGE,
} view_kind_t;

/** What a file name opens in, from its extension. */
view_kind_t view_kind(const char *name);

// ---- text reader ------------------------------------------------------------------------------

/** Open a text file for reading in pages of `rows` lines of `cols` characters. */
bool text_open(const char *path, bool markdown, int cols, int rows, char *err, size_t len);

void text_close(void);

/** Draw the current page with its top-left corner at x, y (FONT_SMALL, `line_h` apart). */
void text_draw(int x, int y, int line_h);

/** Go to page `page` (0 = first). False if the file ends before it. */
bool text_go(int page);

/** Go to the last page. */
void text_end(void);

int text_page(void);

/** True if the current page is the last one. */
bool text_at_end(void);

/** How far through the file the current page ends, 0..100. */
int text_percent(void);

// ---- image viewer -----------------------------------------------------------------------------

/**
 * Show a BMP file on the whole screen, scaled to fit, and flush it. Replaces the whole palette
 * (theme_apply() puts the theme back). `grey` shows colour images in 16 greys.
 */
bool image_show(const char *path, bool grey, char *err, size_t len);

// ---- screenshots ------------------------------------------------------------------------------

#define SHOT_DIR "/pml/shots"

/** Save the screen as SHOT_DIR/shot-NNN.bmp (16 colours). Puts the file's path in `out`. */
bool shot_save(char *out, size_t len);
