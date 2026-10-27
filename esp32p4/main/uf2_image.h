// uf2_image.h - the shared menus' app image API (common/pml), for the ESP32-P4: check a .bin on the
// SD card, flash it into the ota_0 partition and start it. The names are the RP2040/RP2350 ones
// (UF2 files there); here they wrap image_flash.c.
//
//   0x020000 factory (2 MB): this loader | 0x220000 ota_0 (10 MB): the app
//   The custom bootloader (bootloader_components) picks one at reset.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "image_flash.h"

typedef struct {
    uint32_t file_size;
    uint32_t family;       // chip id of the image (ESP_CHIP_ID_ESP32P4)
    uint32_t addr_lo;      // 0
    uint32_t addr_hi;      // bytes written to ota_0
    char name[40];         // project name from the app description
    char version[33];
    char build_date[24];
    image_info_t image;    // where the app is in the file
} uf2_info_t;

typedef void (*uf2_progress_cb_t)(size_t done, size_t total);

/**
 * An app can leave an exit code for the loader: picomultiloader_exit_code() in loader_api.h
 * (LP_SYSTEM store registers 13/14). True (once) if the app that ran last did.
 */
#define PML_EXIT_MAGIC 0x504d4c58u // "PMLX"
bool uf2_exit_code(int32_t *code);

/** Size of the app partition, in bytes. */
uint32_t uf2_flash_capacity(void);

/** "ESP32-P4" */
const char *uf2_chip_name(void);

/** Name of an image's chip id, e.g. "ESP32-P4". */
const char *uf2_family_name(uint32_t family);

/** Check that `path` (a card path, "/dir/app.bin") holds an app for this chip that fits, and read
 *  its description. On failure, a human-readable reason is written to `err`. */
bool uf2_inspect(const char *path, uf2_info_t *info, char *err, size_t err_len);

/** As uf2_inspect(): the check reads only a few headers, so `stop` is not needed. */
bool uf2_inspect_stoppable(const char *path, uf2_info_t *info, bool (*stop)(void), char *err, size_t err_len);

/** Flash the app in `path` into ota_0. On failure, writes the reason to `err`. */
bool uf2_flash(const char *path, const uf2_info_t *info, uf2_progress_cb_t progress, char *err, size_t err_len);

/** Restart into the installed app. Does not return. */
void uf2_boot_installed(void);

/** A 2 KB buffer, free to borrow while no image is inspected or flashed. */
void *uf2_scratch(size_t *len);

// No loader resources in flash on the ESP32-P4: the web page is built in.
typedef enum {
    RES_OK,
    RES_BLANK,
    RES_OLD,
    RES_USED,
    RES_NO_FILE,
} res_state_t;

static inline res_state_t uf2_res_check(void)
{
    return RES_OK;
}

static inline bool uf2_res_ready(void)
{
    return true;
}

static inline bool uf2_res_install(uf2_progress_cb_t progress, char *err, size_t err_len)
{
    (void)progress;
    (void)err;
    (void)err_len;
    return true;
}

/** Restart into the loader again (the bootloader sees the request in LP_SYSTEM store 15). */
void uf2_restart_menu(void) __attribute__((noreturn));

/** Name (and version) of the app currently in ota_0; false if there is none. */
bool uf2_installed(char *name, size_t name_len, char *version, size_t version_len);
