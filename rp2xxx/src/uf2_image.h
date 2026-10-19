// uf2_image.h - check a .uf2 file, flash it with the PicoCalc UF2 Loader (stage3) conventions, or
// hand a RAM-only UF2 to stage3 to run.
//
// PicoMultiLoader runs as the "BOOT2040.UF2" / "BOOT2350.UF2" menu of pelrun's uf2loader: stage3
// stays in flash, loads this menu into RAM, and boots whatever app we leave in the program area.
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
    bool ram_app;          // loads into SRAM (run through stage3 instead of flashing)
    char name[40];         // from binary_info, "" if none
    char version[24];
    char build_date[24];
} uf2_info_t;

typedef void (*uf2_progress_cb_t)(size_t done, size_t total);

/** Call once at startup: map the app partition (RP2350) and read stage3's info. */
void uf2_platform_init(void);

/** True if stage3 left valid bootloader info (needed to flash or run anything). */
bool uf2_bootloader_ok(void);

/** Size of the program area in flash, in bytes (0 if the bootloader info is missing). */
uint32_t uf2_flash_capacity(void);

/** Name of the chip this build is for ("RP2040" / "RP2350"). */
const char *uf2_chip_name(void);

/** Name of a UF2 family ID, e.g. "RP2350 ARM (secure)". */
const char *uf2_family_name(uint32_t family);

/**
 * Check that `path` holds a UF2 for this chip that fits, and read its program info.
 * On failure, a human-readable reason is written to `err`.
 */
bool uf2_inspect(const char *path, uf2_info_t *info, char *err, size_t err_len);

/** Flash the app in `path` into the program area. On failure, writes the reason to `err`. */
bool uf2_flash(const char *path, const uf2_info_t *info, uf2_progress_cb_t progress, char *err, size_t err_len);

/** Reboot into the app in flash (through stage3). Does not return. */
void uf2_boot_installed(void);

/** Ask stage3 to load the RAM app at `path` and run it. Returns only on failure. */
bool uf2_run_ram_app(const char *path, char *err, size_t err_len);

/** Name (and version) of the app currently in flash; false if there is none. */
bool uf2_installed(char *name, size_t name_len, char *version, size_t version_len);
