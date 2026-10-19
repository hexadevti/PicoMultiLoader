// config.h - small key=value settings file on the SD card (the RP2040/RP2350 stand-in for NVS)
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define CONFIG_FILE "/picomultiloader.cfg"

/** Read a setting; false if it isn't set (or the card isn't mounted). */
bool config_get(const char *key, char *buf, size_t len);

/** Store a setting and rewrite the file. An empty value removes it. */
bool config_set(const char *key, const char *value);

/** Forget the cached settings (call after the card was swapped or changed by the PC). */
void config_reload(void);
