// builtin.h - the bundled apps (apps/) built into the loader, on the Tools tab (platforms with
// PML_BUILTIN_APPS: the ESP32-P4). They are the same sources as the separate .uf2 apps of the
// Picos, run with pmlapp_run() in lcd.c's 8-bit mode.
#pragma once

enum { BUILTIN_IMAGE, BUILTIN_MUSIC, BUILTIN_CALC, BUILTINS };

/** Run a built-in app until the user leaves it. False if there was no memory to start it. Redraw
 *  the menu afterwards. */
bool builtin_run(int which);
