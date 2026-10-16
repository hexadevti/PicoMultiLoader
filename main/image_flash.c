#include "image_flash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_flash_partitions.h"
#include "hal/efuse_hal.h"

static const char *TAG = "image";

// Offset of the partition table inside a merged image (esptool merge_bin starts at address 0).
#define MERGED_PTABLE_OFFSET 0x8000
#define CHUNK_SIZE (16 * 1024)

static const char *chip_name(uint16_t id)
{
    switch (id) {
    case ESP_CHIP_ID_ESP32:    return "ESP32";
    case ESP_CHIP_ID_ESP32S2:  return "ESP32-S2";
    case ESP_CHIP_ID_ESP32C3:  return "ESP32-C3";
    case ESP_CHIP_ID_ESP32S3:  return "ESP32-S3";
    case ESP_CHIP_ID_ESP32C2:  return "ESP32-C2";
    case ESP_CHIP_ID_ESP32C6:  return "ESP32-C6";
    case ESP_CHIP_ID_ESP32H2:  return "ESP32-H2";
    case ESP_CHIP_ID_ESP32P4:  return "ESP32-P4";
    case ESP_CHIP_ID_ESP32C5:  return "ESP32-C5";
    case ESP_CHIP_ID_ESP32C61: return "ESP32-C61";
    default:                   return "unknown chip";
    }
}

static bool read_at(FILE *f, size_t offset, void *buf, size_t len)
{
    return fseek(f, (long)offset, SEEK_SET) == 0 && fread(buf, 1, len, f) == len;
}

// Find the app partition (factory first, else the first OTA slot) in a merged image.
static bool find_app_in_merged(FILE *f, size_t file_size, size_t *offset, size_t *max_len)
{
    if (file_size < MERGED_PTABLE_OFFSET + sizeof(esp_partition_info_t)) {
        return false;
    }
    static esp_partition_info_t entries[ESP_PARTITION_TABLE_MAX_ENTRIES];
    size_t n = fseek(f, MERGED_PTABLE_OFFSET, SEEK_SET) == 0
               ? fread(entries, sizeof(entries[0]), ESP_PARTITION_TABLE_MAX_ENTRIES, f) : 0;

    const esp_partition_info_t *best = NULL;
    for (size_t i = 0; i < n && entries[i].magic == ESP_PARTITION_MAGIC; i++) {
        const esp_partition_info_t *e = &entries[i];
        if (e->type != PART_TYPE_APP || (e->pos.offset & 0xFFF) != 0) {
            continue;
        }
        if (e->subtype == PART_SUBTYPE_FACTORY) {
            best = e;
            break;
        }
        if (!best && (e->subtype & PART_SUBTYPE_OTA_FLAG)) {
            best = e;
        }
    }
    if (!best || best->pos.offset >= file_size) {
        return false;
    }
    *offset = best->pos.offset;
    *max_len = file_size - best->pos.offset;
    if (*max_len > best->pos.size) {
        *max_len = best->pos.size;
    }
    return true;
}

// Walk the image segments to get the real image length, so trailing padding is not flashed.
static size_t image_length(FILE *f, size_t offset, const esp_image_header_t *hdr, size_t max_len)
{
    size_t pos = sizeof(esp_image_header_t);
    for (int i = 0; i < hdr->segment_count; i++) {
        esp_image_segment_header_t seg;
        if (pos + sizeof(seg) > max_len || !read_at(f, offset + pos, &seg, sizeof(seg))) {
            return 0;
        }
        pos += sizeof(seg) + seg.data_len;
    }
    pos = (pos + 1 + 15) & ~(size_t)15; // checksum byte, padded to 16
    if (hdr->hash_appended) {
        pos += 32;
    }
    return pos <= max_len ? pos : 0;
}

esp_err_t image_inspect(const char *path, image_info_t *info, char *err, size_t err_len)
{
    memset(info, 0, sizeof(*info));
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, err_len, "cannot open file");
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t ret = ESP_ERR_INVALID_ARG;
    fseek(f, 0, SEEK_END);
    info->file_size = (size_t)ftell(f);

    size_t max_len = info->file_size;
    uint8_t magic = 0;
    if (!read_at(f, 0, &magic, 1)) {
        snprintf(err, err_len, "file is empty");
        goto out;
    }
    if (magic != ESP_IMAGE_HEADER_MAGIC) {
        if (!find_app_in_merged(f, info->file_size, &info->app_offset, &max_len)) {
            snprintf(err, err_len, "not an ESP firmware image");
            goto out;
        }
        info->merged = true;
    }

    if (!read_at(f, info->app_offset, &info->hdr, sizeof(info->hdr)) ||
        info->hdr.magic != ESP_IMAGE_HEADER_MAGIC) {
        snprintf(err, err_len, "no valid image header at 0x%x", (unsigned)info->app_offset);
        goto out;
    }
    if (info->hdr.chip_id != ESP_CHIP_ID_ESP32P4) {
        snprintf(err, err_len, "built for %s, not ESP32-P4", chip_name(info->hdr.chip_id));
        goto out;
    }

    size_t desc_off = info->app_offset + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    if (!read_at(f, desc_off, &info->desc, sizeof(info->desc)) ||
        info->desc.magic_word != ESP_APP_DESC_MAGIC_WORD) {
        snprintf(err, err_len, "not an application image (bootloader or data?)");
        goto out;
    }

    unsigned rev = efuse_hal_chip_revision();
    unsigned min_rev = info->hdr.min_chip_rev_full;
    unsigned max_rev = info->hdr.max_chip_rev_full;
    if (rev < min_rev || (max_rev != 0xFFFF && rev > max_rev)) {
        snprintf(err, err_len, "needs chip rev v%u.%u-v%u.%u, this chip is v%u.%u",
                 min_rev / 100, min_rev % 100, max_rev / 100, max_rev % 100, rev / 100, rev % 100);
        goto out;
    }

    info->app_size = image_length(f, info->app_offset, &info->hdr, max_len);
    if (info->app_size == 0) {
        snprintf(err, err_len, "image is truncated or corrupt");
        goto out;
    }
    ret = ESP_OK;

out:
    fclose(f);
    return ret;
}

esp_err_t image_flash(const char *path, const image_info_t *info, const esp_partition_t *dst,
                      image_progress_cb_t progress)
{
    if (info->app_size > dst->size) {
        ESP_LOGE(TAG, "Image (%u bytes) does not fit in partition %s (%u bytes)",
                 (unsigned)info->app_size, dst->label, (unsigned)dst->size);
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t *buf = malloc(CHUNK_SIZE);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    esp_ota_handle_t ota = 0;
    esp_err_t err = ESP_OK;
    if (fseek(f, (long)info->app_offset, SEEK_SET) != 0) {
        err = ESP_FAIL;
        goto out;
    }

    ESP_LOGI(TAG, "Erasing %s...", dst->label);
    err = esp_ota_begin(dst, info->app_size, &ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        goto out;
    }

    size_t done = 0;
    while (done < info->app_size) {
        size_t want = info->app_size - done;
        if (want > CHUNK_SIZE) {
            want = CHUNK_SIZE;
        }
        if (fread(buf, 1, want, f) != want) {
            ESP_LOGE(TAG, "Read error at offset %u", (unsigned)(info->app_offset + done));
            err = ESP_FAIL;
            break;
        }
        err = esp_ota_write(ota, buf, want);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            break;
        }
        done += want;
        if (progress) {
            progress(done, info->app_size);
        }
    }

    if (err == ESP_OK) {
        err = esp_ota_end(ota); // verifies the image (checksum / SHA256)
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Image verification failed: %s", esp_err_to_name(err));
        }
    } else {
        esp_ota_abort(ota);
    }

out:
    free(buf);
    fclose(f);
    return err;
}
