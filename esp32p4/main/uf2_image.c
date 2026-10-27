// uf2_image.c - the shared menus' app image API on the ESP32-P4 (uf2_image.h), over image_flash.c
#include "uf2_image.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "hal/psram_ctrlr_ll.h"
#include "loader_api.h"
#include "sdcard.h"

static const char *TAG = "image";

static const esp_partition_t *app_partition(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
}

// A card path ("/dir/app.bin") as a VFS path for stdio
static void vfs_path(char *out, size_t len, const char *path)
{
    snprintf(out, len, "%s%s", SDCARD_VFS_ROOT, path);
}

bool uf2_exit_code(int32_t *code)
{
    if (REG_READ(PICOMULTILOADER_EXIT_MAGIC_REG) != PML_EXIT_MAGIC) {
        return false;
    }
    *code = (int32_t)REG_READ(PICOMULTILOADER_EXIT_CODE_REG);
    REG_WRITE(PICOMULTILOADER_EXIT_MAGIC_REG, 0);
    return true;
}

uint32_t uf2_flash_capacity(void)
{
    const esp_partition_t *p = app_partition();
    return p ? p->size : 0;
}

const char *uf2_chip_name(void)
{
    return "ESP32-P4";
}

const char *uf2_family_name(uint32_t family)
{
    switch (family) {
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

bool uf2_inspect(const char *path, uf2_info_t *info, char *err, size_t err_len)
{
    memset(info, 0, sizeof(*info));
    char vfs[300];
    vfs_path(vfs, sizeof(vfs), path);
    if (image_inspect(vfs, &info->image, err, err_len) != ESP_OK) {
        return false;
    }
    const esp_app_desc_t *d = &info->image.desc;
    info->file_size = (uint32_t)info->image.file_size;
    info->family = info->image.hdr.chip_id;
    info->addr_lo = 0;
    info->addr_hi = (uint32_t)info->image.app_size;
    strlcpy(info->name, d->project_name, sizeof(info->name));
    strlcpy(info->version, d->version, sizeof(info->version));
    snprintf(info->build_date, sizeof(info->build_date), "%.11s %.5s", d->date, d->time);
    if (info->addr_hi > uf2_flash_capacity()) {
        snprintf(err, err_len, "too big: %u KB, the app partition holds %u KB", (unsigned)(info->addr_hi / 1024),
                 (unsigned)(uf2_flash_capacity() / 1024));
        return false;
    }
    return true;
}

bool uf2_inspect_stoppable(const char *path, uf2_info_t *info, bool (*stop)(void), char *err, size_t err_len)
{
    (void)stop;
    return uf2_inspect(path, info, err, err_len);
}

bool uf2_flash(const char *path, const uf2_info_t *info, uf2_progress_cb_t progress, char *err, size_t err_len)
{
    const esp_partition_t *dst = app_partition();
    if (!dst) {
        snprintf(err, err_len, "no ota_0 partition");
        return false;
    }
    char vfs[300];
    vfs_path(vfs, sizeof(vfs), path);
    esp_err_t e = image_flash(vfs, &info->image, dst, progress);
    if (e != ESP_OK) {
        snprintf(err, err_len, "%s", esp_err_to_name(e));
        return false;
    }
    return true;
}

void uf2_boot_installed(void)
{
    const esp_partition_t *p = app_partition();
    if (p && esp_ota_set_boot_partition(p) == ESP_OK) {
        ESP_LOGI(TAG, "Booting %s", p->label);
        sdcard_unmount();
        // The loader has no PSRAM, so at power-on IDF gates the PSRAM controller clocks, and
        // esp_restart() (a CPU reset) leaves them gated. An app that uses PSRAM then faults while
        // starting up. Turn them back on first.
        _psram_ctrlr_ll_enable_core_clock(PSRAM_CTRLR_LL_MSPI_ID_2, true);
        _psram_ctrlr_ll_enable_module_clock(PSRAM_CTRLR_LL_MSPI_ID_2, true);
        esp_restart();
    }
    ESP_LOGE(TAG, "No app to boot");
    uf2_restart_menu();
}

void uf2_restart_menu(void)
{
    sdcard_unmount();
    REG_WRITE(PICOMULTILOADER_MAGIC_REG, PICOMULTILOADER_MAGIC);
    esp_restart();
}

bool uf2_installed(char *name, size_t name_len, char *version, size_t version_len)
{
    const esp_partition_t *p = app_partition();
    esp_app_desc_t d;
    if (!p || esp_ota_get_partition_description(p, &d) != ESP_OK) {
        return false;
    }
    strlcpy(name, d.project_name, name_len);
    strlcpy(version, d.version, version_len);
    return true;
}

void *uf2_scratch(size_t *len)
{
    static uint8_t buf[2048] __attribute__((aligned(4)));
    *len = sizeof(buf);
    return buf;
}
