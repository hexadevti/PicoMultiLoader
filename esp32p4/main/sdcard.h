// sdcard.h - the PicoCalc's microSD slot (SPI3) with FatFs on top.
//
// The card is FatFs drive 0 and is also mounted in the VFS at SDCARD_VFS_ROOT. The shared code
// (common/pml) calls f_* with "/dir/file" paths; ESP-IDF code (image_flash.c, the web server)
// uses stdio on SDCARD_VFS_ROOT "/dir/file".
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "ff.h"
#include "sdmmc_cmd.h"

// FatFs paths are "/dir/file"; the UI keeps "" for the root so "<root>/<name>" works unchanged.
#define SDCARD_MOUNT_POINT ""
#define SDCARD_VFS_ROOT    "/sdcard"

/** There is no card-detect switch: always true. */
bool sdcard_present(void);

/** Initialise the card and mount the FAT filesystem. */
bool sdcard_mount(void);

void sdcard_unmount(void);

/** Free and total space in bytes (false if not mounted). */
bool sdcard_space(uint64_t *total, uint64_t *free_bytes);

/** sdcard_space() without blocking, for a card whose free space isn't known yet (counting it reads
 *  the whole FAT, here in a background task): 1 = done, filled in; 0 = not yet, call again;
 *  -1 = can't tell. */
int sdcard_space_step(uint64_t *total, uint64_t *free_bytes);

/** Unmount the filesystem and open the card for raw sector access (USB drive mode). */
esp_err_t sdcard_open_raw(sdmmc_card_t **card);

/** Release the card opened with sdcard_open_raw(). */
void sdcard_close_raw(void);

/** Turn "" into "/" for FatFs calls. */
static inline const char *sd_path(const char *p)
{
    return p[0] ? p : "/";
}
