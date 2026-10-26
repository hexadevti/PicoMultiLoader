// sdcard.c - the microSD slot on SPI3 (sdcard.h)
#include "sdcard.h"

#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "esp_ldo_regulator.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "sdmmc_cmd.h"
#include "board.h"

static const char *TAG = "sdcard";

static sdmmc_card_t *s_card;
static sdmmc_card_t *s_raw_card;
static sdspi_dev_handle_t s_raw_dev;
static esp_ldo_channel_handle_t s_ldo;
static bool s_bus_ok;

static esp_err_t power_and_bus_init(void)
{
    if (!s_ldo) {
        esp_ldo_channel_config_t ldo_cfg = {
            .chan_id = SD_IO_LDO_CHAN,
            .voltage_mv = 3300,
        };
        esp_err_t err = esp_ldo_acquire_channel(&ldo_cfg, &s_ldo);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to enable IO LDO %d: %s", SD_IO_LDO_CHAN, esp_err_to_name(err));
            return err;
        }
    }

    if (!s_bus_ok) {
        spi_bus_config_t bus = {
            .mosi_io_num = SD_MOSI_PIN,
            .miso_io_num = SD_MISO_PIN,
            .sclk_io_num = SD_SCK_PIN,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 4096,
        };
        esp_err_t err = spi_bus_initialize(SD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to init SD SPI bus: %s", esp_err_to_name(err));
            return err;
        }
        s_bus_ok = true;
    }
    return ESP_OK;
}

static void host_config(sdmmc_host_t *host, sdspi_device_config_t *slot)
{
    *host = (sdmmc_host_t)SDSPI_HOST_DEFAULT();
    host->slot = SD_SPI_HOST;
    host->max_freq_khz = SD_SPI_KHZ;

    *slot = (sdspi_device_config_t)SDSPI_DEVICE_CONFIG_DEFAULT();
    slot->gpio_cs = SD_CS_PIN;
    slot->host_id = SD_SPI_HOST;
}

bool sdcard_present(void)
{
    return true;
}

bool sdcard_mount(void)
{
    if (s_card) {
        return true;
    }
    if (s_raw_card) {
        return false; // lent to USB
    }
    esp_err_t err = power_and_bus_init();
    if (err != ESP_OK) {
        return false;
    }

    sdmmc_host_t host;
    sdspi_device_config_t slot;
    host_config(&host, &slot);

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };

    err = esp_vfs_fat_sdspi_mount(SDCARD_VFS_ROOT, &host, &slot, &mount_config, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(err));
        s_card = NULL;
        return false;
    }
    sdmmc_card_print_info(stdout, s_card);
    return true;
}

// ---- free space -------------------------------------------------------------------------------
// The first f_getfree() on a big card reads the whole FAT (seconds): a background task does it.
// It holds the volume lock meanwhile, so file calls from the menu wait for it rather than clash.
typedef enum { SPACE_IDLE, SPACE_COUNTING, SPACE_DONE, SPACE_FAILED } space_state_t;
static volatile space_state_t s_space_state;

static void space_task(void *arg)
{
    uint64_t total, free_bytes;
    s_space_state = sdcard_space(&total, &free_bytes) ? SPACE_DONE : SPACE_FAILED;
    vTaskDelete(NULL);
}

void sdcard_unmount(void)
{
    while (s_space_state == SPACE_COUNTING) { // let the count finish with the card
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_space_state = SPACE_IDLE;
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(SDCARD_VFS_ROOT, s_card);
        s_card = NULL;
    }
}

bool sdcard_space(uint64_t *total, uint64_t *free_bytes)
{
    FATFS *fs;
    DWORD free_clusters;
    if (!s_card || f_getfree("", &free_clusters, &fs) != FR_OK) {
        return false;
    }
#if FF_MAX_SS != FF_MIN_SS
    uint64_t cluster = (uint64_t)fs->csize * fs->ssize;
#else
    uint64_t cluster = (uint64_t)fs->csize * FF_MAX_SS;
#endif
    *total = (uint64_t)(fs->n_fatent - 2) * cluster;
    *free_bytes = (uint64_t)free_clusters * cluster;
    return true;
}

int sdcard_space_step(uint64_t *total, uint64_t *free_bytes)
{
    if (!s_card) {
        return -1;
    }
    switch (s_space_state) {
    case SPACE_IDLE: {
        FF_DIR d; // the volume, without counting: known already (FSINFO)?
        FATFS *fs = NULL;
        if (f_opendir(&d, "/") == FR_OK) {
            fs = d.obj.fs;
            f_closedir(&d);
        }
        if (fs && fs->free_clst <= fs->n_fatent - 2) {
            return sdcard_space(total, free_bytes) ? 1 : -1;
        }
        s_space_state = SPACE_COUNTING;
        if (xTaskCreate(space_task, "sdspace", 3072, NULL, 2, NULL) != pdPASS) {
            s_space_state = SPACE_IDLE;
            return sdcard_space(total, free_bytes) ? 1 : -1;
        }
        return 0;
    }
    case SPACE_COUNTING:
        return 0;
    case SPACE_DONE:
        return sdcard_space(total, free_bytes) ? 1 : -1; // counted: fast now
    default:
        s_space_state = SPACE_IDLE;
        return -1;
    }
}

esp_err_t sdcard_open_raw(sdmmc_card_t **card)
{
    if (s_raw_card) {
        *card = s_raw_card;
        return ESP_OK;
    }
    sdcard_unmount();
    esp_err_t err = power_and_bus_init();
    if (err != ESP_OK) {
        return err;
    }

    sdmmc_host_t host;
    sdspi_device_config_t slot;
    host_config(&host, &slot);
    sdspi_host_init();
    err = sdspi_host_init_device(&slot, &s_raw_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD SPI device init failed: %s", esp_err_to_name(err));
        return err;
    }
    host.slot = s_raw_dev;

    sdmmc_card_t *c = calloc(1, sizeof(*c));
    if (!c) {
        sdspi_host_remove_device(s_raw_dev);
        return ESP_ERR_NO_MEM;
    }
    err = sdmmc_card_init(&host, c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD card init failed: %s", esp_err_to_name(err));
        free(c);
        sdspi_host_remove_device(s_raw_dev);
        return err;
    }
    s_raw_card = *card = c;
    return ESP_OK;
}

void sdcard_close_raw(void)
{
    if (s_raw_card) {
        sdspi_host_remove_device(s_raw_dev);
        free(s_raw_card);
        s_raw_card = NULL;
    }
}
