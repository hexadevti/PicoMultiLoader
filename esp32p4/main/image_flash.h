#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_app_format.h"
#include "esp_app_desc.h"
#include "esp_partition.h"

typedef struct {
    size_t file_size;
    size_t app_offset;      // where the app image starts inside the file
    size_t app_size;        // bytes to write to the app partition
    bool merged;            // file is a merged image (bootloader + partition table + app)
    esp_image_header_t hdr;
    esp_app_desc_t desc;
} image_info_t;

typedef void (*image_progress_cb_t)(size_t done, size_t total);

/**
 * Check that `path` holds an ESP32-P4 application this chip can run, and find
 * the app image inside it (plain app .bin or merged flash image).
 * On failure, a human-readable reason is written to `err`.
 */
esp_err_t image_inspect(const char *path, image_info_t *info, char *err, size_t err_len);

/** Write the app image described by `info` into `dst` and verify it. */
esp_err_t image_flash(const char *path, const image_info_t *info, const esp_partition_t *dst,
                      image_progress_cb_t progress);
