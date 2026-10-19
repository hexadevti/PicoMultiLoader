// sdcard.h - the PicoCalc's microSD slot (SPI mode) with FatFS on top
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "ff.h"

// FatFS paths are "/dir/file"; the UI keeps "" for the root so "<root>/<name>" works unchanged.
#define SDCARD_MOUNT_POINT ""

/** True if the card-detect switch says a card is in the slot. */
bool sdcard_present(void);

/** Initialise the card and mount the FAT filesystem. */
bool sdcard_mount(void);

void sdcard_unmount(void);

/** Free and total space in bytes (false if not mounted). */
bool sdcard_space(uint64_t *total, uint64_t *free_bytes);

/** Unmount the filesystem and open the card for raw sector access (USB drive mode). */
bool sdcard_open_raw(uint32_t *sector_count);

/** Release the card opened with sdcard_open_raw(). */
void sdcard_close_raw(void);

/** Raw 512-byte sector access (for FatFS and USB drive mode). */
bool sd_read_sectors(uint32_t lba, uint8_t *buf, uint32_t count);
bool sd_write_sectors(uint32_t lba, const uint8_t *buf, uint32_t count);

/** Turn "" into "/" for FatFS calls. */
static inline const char *sd_path(const char *p)
{
    return p[0] ? p : "/";
}
