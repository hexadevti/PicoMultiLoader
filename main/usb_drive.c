// usb_drive.c - SD card as a USB mass-storage device.
//
// The TinyUSB MSC callbacks are implemented here on raw SD sectors. This replaces esp_tinyusb's
// tusb_msc_storage.c (it is never linked because every symbol it provides is defined here), whose
// mount logic would *format* the card if FatFs ever failed to mount it.
#include "usb_drive.h"

#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "tinyusb.h"
#include "class/msc/msc_device.h"
#include "sdcard.h"

static const char *TAG = "usbdrive";

static bool s_installed;
static volatile bool s_exposed;
static volatile bool s_ejected;
static sdmmc_card_t *s_card;
static SemaphoreHandle_t s_lock; // held while a USB transfer touches the card

esp_err_t usbdrive_start(void)
{
    if (s_exposed) {
        return ESP_OK;
    }
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    esp_err_t err = sdcard_open_raw(&s_card);
    if (err != ESP_OK) {
        sdcard_mount();
        return err;
    }
    s_ejected = false;
    s_exposed = true;

    if (!s_installed) {
        const tinyusb_config_t cfg = {0}; // default descriptors from Kconfig
        err = tinyusb_driver_install(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "USB driver install failed: %s", esp_err_to_name(err));
            usbdrive_stop();
            return err;
        }
        s_installed = true;
    } else {
        tud_connect();
    }
    ESP_LOGI(TAG, "SD card exposed over USB (%u MB)", (unsigned)usbdrive_size_mb());
    return ESP_OK;
}

void usbdrive_stop(void)
{
    s_exposed = false;
    if (s_installed) {
        tud_disconnect();
    }
    if (s_lock) { // wait for a transfer in flight
        xSemaphoreTake(s_lock, portMAX_DELAY);
        xSemaphoreGive(s_lock);
    }
    s_card = NULL;
    sdcard_close_raw();
    sdcard_mount();
    ESP_LOGI(TAG, "SD card back in the loader");
}

usbdrive_state_t usbdrive_state(void)
{
    if (!s_exposed) {
        return USBDRIVE_OFF;
    }
    if (s_ejected) {
        return USBDRIVE_EJECTED;
    }
    if (!tud_mounted()) {
        return USBDRIVE_WAITING;
    }
    return tud_suspended() ? USBDRIVE_SUSPENDED : USBDRIVE_CONNECTED;
}

uint32_t usbdrive_size_mb(void)
{
    return s_card ? (uint32_t)((uint64_t)s_card->csd.capacity * s_card->csd.sector_size / (1024 * 1024)) : 0;
}

// ---- TinyUSB MSC callbacks (run in the TinyUSB task) ------------------------------------------
void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
{
    memcpy(vendor_id, "ESP32-P4", 8);
    memcpy(product_id, "PicoCalc SD card", 16);
    memcpy(product_rev, "1.0 ", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    if (!s_exposed || s_ejected) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00); // medium not present
        return false;
    }
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    *block_count = s_card ? (uint32_t)s_card->csd.capacity : 0;
    *block_size = s_card ? (uint16_t)s_card->csd.sector_size : 512;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    if (load_eject && !start) {
        ESP_LOGI(TAG, "Ejected by the PC");
        s_ejected = true;
    }
    return true;
}

static int32_t transfer(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize, bool write)
{
    if (!s_exposed || !s_card || offset != 0 || bufsize % s_card->csd.sector_size) {
        return -1;
    }
    size_t count = bufsize / s_card->csd.sector_size;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = !s_exposed ? ESP_ERR_INVALID_STATE
                    : write    ? sdmmc_write_sectors(s_card, buffer, lba, count)
                               : sdmmc_read_sectors(s_card, buffer, lba, count);
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s lba %u x%u failed: %s", write ? "write" : "read", (unsigned)lba, (unsigned)count,
                 esp_err_to_name(err));
        return -1;
    }
    return (int32_t)bufsize;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    return transfer(lba, offset, buffer, bufsize, false);
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    return transfer(lba, offset, buffer, bufsize, true);
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize)
{
    if (scsi_cmd[0] == SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL) {
        return 0;
    }
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00); // invalid command
    return -1;
}

// tusb_msc_storage.c also defines these; defining them here keeps it out of the link.
void tud_mount_cb(void)
{
}

void tud_umount_cb(void)
{
}
