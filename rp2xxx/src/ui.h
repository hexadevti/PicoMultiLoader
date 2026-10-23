// ui.h - the on-screen launcher: Home (the apps), the file list, Setup, Wi-Fi and USB screens
#pragma once

#include <stdbool.h>

/** Load the colour theme and show the boot screen, with the SD card as its first checklist line. */
void ui_boot(bool sd_ok);

/** Add a line to the boot screen's checklist: what was checked, what was found, and if that's good. */
void ui_boot_check(const char *what, const char *result, bool ok);

/** Load the theme (if ui_boot() didn't), the starting directory, and draw the first screen. */
void ui_init(bool sd_ok);

/** Handle one key (keyboard.h codes). May flash an app and reboot. */
void ui_key(int key);

/** Redraw the current screen (e.g. after the panel was re-initialised). */
void ui_redraw(void);

/** Call regularly from the main loop: follows the USB drive and web server state. */
void ui_tick(void);

/** Update the battery indicator; redraws if it changed. */
void ui_set_battery(int percent, bool charging);
