// plat.h - what the shared code (common/pml) needs from the platform: the RP2040 / RP2350 build.
// Time is time_us_64() and sleep_ms() from the Pico SDK.
#pragma once

#include <malloc.h>
#include "pico/stdlib.h"
#include "hardware/watchdog.h"

#if PICO_RP2040
#define PML_RP2040 1
#define PML_RP2350 0
#else
#define PML_RP2040 0
#define PML_RP2350 1
#endif
#define PML_P4 0

// The Wi-Fi firmware and the web page in the last 256 KB of flash, installed from the loader file
// (uf2_res_*, RP2350 with Wi-Fi only)
#define PML_RES (PICOMULTILOADER_WIFI && PML_RP2350)

// Apps are UF2 files
#define PML_APP_EXT ".uf2"
// The menu can switch to the other style (RP2350: the boot stub loads either from the SD card)
#define PML_MENU_SWITCH PML_RP2350
// The menus are files on the SD card (pml2350.uf2, pml2350-classic.uf2), protected from deletion
#define PML_MENU_FILES PML_RP2350

// Help text about the USB ports
#define PML_USB_HELP    "Needs a cable on the Pico's micro-USB port; the PicoCalc's USB-C port is serial only."
#define PML_USB_WAIT    "Connect the Pico's micro-USB port to the PC. The PicoCalc's USB-C port is serial only."
#define PML_SERIAL_PORT "PicoCalc's USB-C port"

/** Free and total heap in bytes. */
static inline void plat_heap(size_t *free_bytes, size_t *total)
{
    extern char __end__, __HeapLimit;
    struct mallinfo mi = mallinfo();
    *total = (size_t)(&__HeapLimit - &__end__);
    *free_bytes = *total - mi.arena + mi.fordblks;
}

/** Restart the chip (the loader, or the app, as the boot path decides). */
static inline void __attribute__((noreturn)) plat_reboot(void)
{
    watchdog_reboot(0, 0, 0);
    for (;;) {
    }
}
