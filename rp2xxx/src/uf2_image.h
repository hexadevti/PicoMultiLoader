// uf2_image.h - check a .uf2 file, flash it into the app area and start it.
//
// PicoMultiLoader is the firmware in flash (no other bootloader). It starts at power-on, decides
// whether to show the menu or start the installed app, and flashes apps behind itself:
//
//   RP2040 (2 MB):  0x000000 boot2 (ours) + app ... 0x1BF000 app record | 0x1C0000 loader (256 KB)
//   RP2350 (4 MB):  0x000000 boot stub (stub/stub.c) | 0x002000 app (mapped to 0x10000000) ...
//                   0x3C0000 loader resources (256 KB: Wi-Fi firmware, web page);
//                   the loader itself runs from RAM (pml2350.uf2 on the SD card)
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t file_size;
    uint32_t family;       // UF2 family ID of the blocks used
    uint32_t num_blocks;   // blocks for this chip
    uint32_t addr_lo;      // lowest target address
    uint32_t addr_hi;      // end of the highest block
    bool ram_app;          // loads into SRAM (no_flash build): not supported
    char name[40];         // from binary_info, "" if none
    char version[24];
    char build_date[24];
} uf2_info_t;

typedef void (*uf2_progress_cb_t)(size_t done, size_t total);

/**
 * Call first thing at power-on. Starts the installed app (does not return) unless the menu is
 * wanted: Up, F1 or F5 held on the keyboard, an app asked for the menu (uf2loader's watchdog
 * request), or no valid app is installed.
 */
void uf2_boot_or_menu(void);

/**
 * An app can leave an exit code for the loader: watchdog scratch[2] = PML_EXIT_MAGIC, scratch[3] =
 * the code, then reboot. True (once) if the app that ran last did.
 */
#define PML_EXIT_MAGIC 0x504d4c58u // "PMLX"
bool uf2_exit_code(int32_t *code);

/** Size of the app area in flash, in bytes. */
uint32_t uf2_flash_capacity(void);

/** Name of the chip this build is for ("RP2040" / "RP2350"). */
const char *uf2_chip_name(void);

/** Name of a UF2 family ID, e.g. "RP2350 ARM". */
const char *uf2_family_name(uint32_t family);

/**
 * Check that `path` holds a UF2 for this chip that fits, and read its program info.
 * On failure, a human-readable reason is written to `err`.
 */
bool uf2_inspect(const char *path, uf2_info_t *info, char *err, size_t err_len);

/** As uf2_inspect(), but `stop()` is asked now and then; if it says true, the check is given up and
 *  false is returned with `err` empty. */
bool uf2_inspect_stoppable(const char *path, uf2_info_t *info, bool (*stop)(void), char *err, size_t err_len);

/** Flash the app in `path` into the app area. On failure, writes the reason to `err`. */
bool uf2_flash(const char *path, const uf2_info_t *info, uf2_progress_cb_t progress, char *err, size_t err_len);

/** Reboot straight into the installed app. Does not return. */
void uf2_boot_installed(void);

/** The file walk's read buffer (2 KB), free to borrow while no UF2 is inspected or flashed. */
void *uf2_scratch(size_t *len);

#if !PICO_RP2040
/** The loader's resources in the last 256 KB of flash (RP2350; Wi-Fi needs them). */
typedef enum {
    RES_OK,      // installed, and the ones this loader was built with
    RES_BLANK,   // the area is erased: install them
    RES_OLD,     // another loader version's, or an interrupted install: install them
    RES_USED,    // something else is there (an app's data): installing erases it, ask first
    RES_NO_FILE, // this build has no resources (add_res.py did not run)
} res_state_t;

/** The state of the resource area (checked once, then cached). */
res_state_t uf2_res_check(void);

/** uf2_res_check() == RES_OK. */
bool uf2_res_ready(void);

/** Copy the resources from /pml2350.uf2 into flash (a few seconds). On failure, writes `err`. */
bool uf2_res_install(uf2_progress_cb_t progress, char *err, size_t err_len);
#endif

/** Name (and version) of the app currently in flash; false if there is none. */
bool uf2_installed(char *name, size_t name_len, char *version, size_t version_len);
