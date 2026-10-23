// look.h - colour themes and palette-cycling effects for the 16-colour framebuffer.
// A theme fills palette entries 0..12; entries 13..15 rotate between the accent colours a few
// times a second, so anything drawn in them (marching borders, striped bars, the boot logo)
// moves without being redrawn: look_tick() only re-sends the rows that use them.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lcd.h"

// Palette roles
enum {
    C_BG,
    C_PANEL,  // status and key bars
    C_FIELD,  // raised things on a panel: text fields, key caps, toasts
    C_LINE,   // rules and frames
    C_DIM,
    C_FG,
    C_BRIGHT,
    C_ACCENT,
    C_ACCDK,  // dark accent
    C_OK,
    C_WARN,
    C_ERR,
    C_ACC2,   // second accent
    C_CYC0,   // 13..15: cycled
};
#define C_SEL_BG  C_ACCENT
#define C_SEL_FG  C_BG
#define C_SIG_OFF C_LINE

#define THEME_DIR "/pml/themes"

/**
 * Load a theme: /pml/themes/<name>.pal if there is one, else the built-in theme of that name, else
 * the first built-in one. Returns false in that last case. Doesn't redraw.
 */
bool theme_load(const char *name);

/** Put the loaded theme's colours back in the palette (after a screen that used its own). */
void theme_apply(void);

/** Name of the loaded theme. */
const char *theme_name(void);

/** The theme after the loaded one: the built-ins, then the .pal files in /pml/themes. */
void theme_next(char *out, size_t len);

/** Forget which rows use the cycled colours (call before drawing a whole screen). */
void look_anim_reset(void);

void look_frame(int x, int y, int w, int h, lcd_color_t color);

/** A 1-pixel border in the cycled colours: it marches. */
void look_march(int x, int y, int w, int h);

/** Diagonal stripes in the cycled colours: they slide. */
void look_stripes(int x, int y, int w, int h);

/** Checkerboard rows y..y+h-1 with C_BG, dimming what is behind a dialog. */
void look_dither(int y, int h);

/** Rotate the cycled colours once (the caller flushes). */
void look_step(void);

/** Call from the main loop: steps the cycle every 120 ms when something on screen uses it. */
void look_tick(void);
