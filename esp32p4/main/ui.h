// ui.h - the on-screen file browser (PicoCalc UF2 Loader style)
#pragma once

#include <stdbool.h>

/** Load the starting directory and draw the first screen. */
void ui_init(bool sd_ok);

/** Handle one key (keyboard.h codes). May flash an app and reboot. */
void ui_key(int key);

/** Redraw the current screen (e.g. after the panel was re-initialised). */
void ui_redraw(void);

/** Call regularly from the main loop: follows the USB drive state while that screen is open. */
void ui_tick(void);

/** Update the battery indicator; redraws if it changed. */
void ui_set_battery(int percent, bool charging);
