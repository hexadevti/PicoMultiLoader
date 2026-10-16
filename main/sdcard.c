#include "sdcard.h"

#include <stdlib.h>
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

esp_err_t sdcard_mount(void)
{
    if (s_card) {
        return ESP_OK;
    }
    if (s_raw_card) {
        return ESP_ERR_INVALID_STATE; // lent to USB
    }
    esp_err_t err = power_and_bus_init();
    if (err != ESP_OK) {
        return err;
    }

    sdmmc_host_t host;
    sdspi_device_config_t slot;
    host_config(&host, &slot);

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };

    err = esp_vfs_fat_sdspi_mount(SDCARD_MOUNT_POINT, &host, &slot, &mount_config, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(err));
        s_card = NULL;
        return err;
    }
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

void sdcard_unmount(void)
{
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(SDCARD_MOUNT_POINT, s_card);
        s_card = NULL;
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
