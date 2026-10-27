// usb_drive.h - share the SD card with a PC as a USB mass-storage drive (the Pico's micro-USB port)
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    USBDRIVE_OFF,
    USBDRIVE_WAITING,   // exposed, but no PC has enumerated us (cable not connected)
    USBDRIVE_CONNECTED, // a PC has the drive
    USBDRIVE_SUSPENDED, // the bus went quiet: cable unplugged or the PC is asleep
    USBDRIVE_EJECTED,   // the PC ejected the drive; safe to take the card back
} usbdrive_state_t;

/** Unmount the SD card from the loader and expose it over USB. */
bool usbdrive_start(void);

/** Stop exposing the card and remount it in the loader. */
void usbdrive_stop(void);

/** Run the USB stack; call often from the main loop. */
void usbdrive_task(void);

usbdrive_state_t usbdrive_state(void);

/** Card size in MB (valid after usbdrive_start()). */
uint32_t usbdrive_size_mb(void);
