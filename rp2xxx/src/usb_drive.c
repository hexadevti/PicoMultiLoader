// usb_drive.c - SD card as a USB mass-storage device, with TinyUSB's MSC class on raw SD sectors.
#include "usb_drive.h"

#include <stdio.h>
#include <string.h>
#include "tusb.h"
#include "config.h"
#include "sdcard.h"

static bool s_installed;
static bool s_exposed;
static bool s_ejected;
static uint32_t s_sectors;

bool usbdrive_start(void)
{
    if (s_exposed) {
        return true;
    }
    if (!sdcard_open_raw(&s_sectors)) {
        sdcard_mount();
        return false;
    }
    s_ejected = false;
    s_exposed = true;
    if (!s_installed) {
        tud_init(BOARD_TUD_RHPORT);
        s_installed = true;
    } else {
        tud_connect();
    }
    printf("usb: SD card exposed (%u MB)\n", (unsigned)usbdrive_size_mb());
    return true;
}

void usbdrive_stop(void)
{
    s_exposed = false;
    if (s_installed) {
        tud_disconnect();
    }
    sdcard_close_raw();
    sdcard_mount();
    config_reload(); // the PC may have changed the settings file
    printf("usb: SD card back in the loader\n");
}

void usbdrive_task(void)
{
    if (s_installed) {
        tud_task();
    }
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
    return s_sectors / 2048;
}

// ---- TinyUSB MSC callbacks (run from tud_task) ------------------------------------------------
void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
{
    (void)lun;
    memcpy(vendor_id, "PicoCalc", 8);
    memcpy(product_id, "SD card         ", 16);
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
    (void)lun;
    *block_count = s_exposed ? s_sectors : 0;
    *block_size = 512;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    (void)lun;
    (void)power_condition;
    if (load_eject && !start) {
        printf("usb: ejected by the PC\n");
        s_ejected = true;
    }
    return true;
}

static int32_t transfer(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize, bool write)
{
    if (!s_exposed || offset != 0 || bufsize % 512) {
        return -1;
    }
    uint32_t count = bufsize / 512;
    bool ok = write ? sd_write_sectors(lba, buffer, count) : sd_read_sectors(lba, buffer, count);
    if (!ok) {
        printf("usb: %s lba %lu x%lu failed\n", write ? "write" : "read", (unsigned long)lba, (unsigned long)count);
        return -1;
    }
    return (int32_t)bufsize;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    (void)lun;
    return transfer(lba, offset, buffer, bufsize, false);
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    (void)lun;
    return transfer(lba, offset, buffer, bufsize, true);
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize)
{
    (void)buffer;
    (void)bufsize;
    if (scsi_cmd[0] == SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL) {
        return 0;
    }
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00); // invalid command
    return -1;
}
