// plat.h - what the shared code (common/pml) needs from the platform: the ESP32-P4 build.
// time_us_64(), sleep_ms() and sleep_us() stand in for the Pico SDK's.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"

#define PML_RP2040 0
#define PML_RP2350 0
#define PML_P4     1

// No Wi-Fi files in flash to install: the web page is built in, the C6 has its own firmware
#define PML_RES 0

// Apps are ESP-IDF images
#define PML_APP_EXT ".bin"
// One firmware holds both menus: the "ui" setting picks one at start (ui_dispatch.c)
#define PML_MENU_SWITCH 1
#define PML_MENU_FILES  0

// Help text about the USB ports
#define PML_USB_HELP    "Needs a cable on the board's 4-pin USB OTG header (VBUS, D-, D+, GND); the USB-C port is serial only."
#define PML_USB_WAIT    "Connect the 4-pin USB OTG header (VBUS, D-, D+, GND) to the PC. The USB-C port is serial only."
#define PML_SERIAL_PORT "USB-C port"

static inline uint64_t time_us_64(void)
{
    return (uint64_t)esp_timer_get_time();
}

static inline void sleep_us(uint32_t us)
{
    esp_rom_delay_us(us);
}

/** Sleep, letting the other tasks (Wi-Fi, USB, the idle task) run. */
static inline void sleep_ms(uint32_t ms)
{
    TickType_t t = pdMS_TO_TICKS(ms);
    vTaskDelay(t ? t : 1);
}

/** Free and total heap in bytes (internal RAM). */
static inline void plat_heap(size_t *free_bytes, size_t *total)
{
    *free_bytes = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    *total = heap_caps_get_total_size(MALLOC_CAP_8BIT);
}

/** Restart the chip (the loader, or the app, as the bootloader decides). */
static inline void __attribute__((noreturn)) plat_reboot(void)
{
    esp_restart();
}
