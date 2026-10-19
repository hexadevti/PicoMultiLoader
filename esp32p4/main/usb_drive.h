// usb_drive.h - share the SD card with a PC as a USB mass-storage drive (ESP32-P4 USB OTG HS port)
#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef enum {
    USBDRIVE_OFF,
    USBDRIVE_WAITING,   // exposed, but no PC has enumerated us (cable not connected)
    USBDRIVE_CONNECTED, // a PC has the drive
    USBDRIVE_SUSPENDED, // the bus went quiet: cable unplugged or the PC is asleep
    USBDRIVE_EJECTED,   // the PC ejected the drive; safe to take the card back
} usbdrive_state_t;

/** Unmount the SD card from the loader and expose it over USB. */
esp_err_t usbdrive_start(void);

/** Stop exposing the card and remount it in the loader. */
void usbdrive_stop(void);

usbdrive_state_t usbdrive_state(void);

/** Card size in MB (valid after usbdrive_start()). */
uint32_t usbdrive_size_mb(void);
