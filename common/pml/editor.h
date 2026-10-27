// editor.h - the text editor (Home > Tools, E in the text reader, Tab > Edit in Files). The text is
// held in memory only while a file is open; ui.c owns the screen and the prompts, this edits.
#pragma once

#include <stdbool.h>
#include <stddef.h>

/** Start editing `path` ("" = a new, untitled text) in a window of `cols` x `rows` characters. */
bool ed_open(const char *path, int cols, int rows, char *err, size_t len);

/** Drop the text and its memory (unsaved changes are lost: ask first). */
void ed_close(void);

/** Draw the text with its top-left corner at x, y (FONT_SMALL, `line_h` apart), line numbers on the left. */
void ed_draw(int x, int y, int line_h);

/** A moving or editing key. False if the key isn't one. */
bool ed_key(int key);

/** "Ln 3/40  Col 5  1.2 KB  LF" */
void ed_status(char *out, size_t len);

/** The file's path, "" while untitled. */
const char *ed_path(void);

bool ed_modified(void);

/** Save to `path` (NULL or "" = the file's own path). */
bool ed_save(const char *path, char *err, size_t len);

/** Find `what` (not case-sensitive) after the cursor, going round to the start. */
bool ed_find(const char *what);

/** Go to line `n` (0 = the end). */
void ed_goto_line(long n);

/** A message for the user from the last key ("The file is full."), or NULL. Cleared when read. */
const char *ed_take_message(void);
