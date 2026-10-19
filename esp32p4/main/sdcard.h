#pragma once

#include "esp_err.h"
#include "sdmmc_cmd.h"

#define SDCARD_MOUNT_POINT "/sdcard"

esp_err_t sdcard_mount(void);
void sdcard_unmount(void);

/** Unmount the filesystem and open the card for raw sector access (USB drive mode). */
esp_err_t sdcard_open_raw(sdmmc_card_t **card);

/** Release the card opened with sdcard_open_raw(). */
void sdcard_close_raw(void);
