/*
 * SPDX-FileCopyrightText: 2015-2021 Espressif Systems (Shanghai) CO LTD
 *
 * Custom second-stage bootloader for PicoMultiLoader. Taken from ESP-IDF v5.5's
 * examples/custom_bootloader/bootloader_override (see THIRD-PARTY.md).
 *
 * Identical to the stock ESP-IDF bootloader, except that before booting the installed app it
 * checks whether the loader ("factory" partition) was requested:
 *   - an app wrote LOADER_MAGIC to LP_STORE15 and rebooted (see main/loader_api.h), or
 *   - within CONFIG_LOADER_TRIGGER_WINDOW_MS of reset, the BOOT button was pressed, or
 *     Up / F1 / F5 was pressed on the PicoCalc keyboard (like the PicoCalc UF2 Loader).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdbool.h>
#include <sys/reent.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "soc/gpio_periph.h"
#include "soc/gpio_sig_map.h"
#include "soc/lp_system_reg.h"
#include "hal/gpio_ll.h"
#include "bootloader_init.h"
#include "bootloader_utility.h"
#include "bootloader_common.h"

// Keep in sync with main/loader_api.h
#define LOADER_MAGIC_REG LP_SYSTEM_REG_LP_STORE15_REG
#define LOADER_MAGIC     0x10ADE125u

static const char *TAG = "boot";

static int select_partition_number(bootloader_state_t *bs);
static bool loader_requested(void);

void __attribute__((noreturn)) call_start_cpu0(void)
{
    // 1. Hardware initialization
    if (bootloader_init() != ESP_OK) {
        bootloader_reset();
    }

#ifdef CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP
    bootloader_utility_load_boot_image_from_deep_sleep();
#endif

    // 2. Select the number of boot partition
    bootloader_state_t bs = {0};
    int boot_index = select_partition_number(&bs);
    if (boot_index == INVALID_INDEX) {
        bootloader_reset();
    }

    // 2.1 Divert to the loader if requested
    bool by_magic = REG_READ(LOADER_MAGIC_REG) == LOADER_MAGIC;
    REG_WRITE(LOADER_MAGIC_REG, 0);
    if (boot_index != FACTORY_INDEX && bs.factory.offset != 0 && (by_magic || loader_requested())) {
        ESP_LOGI(TAG, "Loader requested, booting factory partition");
        boot_index = FACTORY_INDEX;
    }

    // 3. Load the app image for booting
    bootloader_utility_load_boot_image(&bs, boot_index);
}

static int select_partition_number(bootloader_state_t *bs)
{
    if (!bootloader_utility_load_partition_table(bs)) {
        ESP_LOGE(TAG, "load partition table error!");
        return INVALID_INDEX;
    }
    return bootloader_utility_get_selected_boot_partition(bs);
}

static void pin_input_pullup(uint32_t pin)
{
    esp_rom_gpio_pad_select_gpio(pin);
    if (((1ULL << pin) & SOC_GPIO_VALID_GPIO_MASK) != 0) {
        gpio_ll_input_enable(&GPIO, pin);
    }
    esp_rom_gpio_pad_pullup_only(pin);
}

#if CONFIG_LOADER_KBD_ENABLE
// ---- Minimal bit-banged I2C master for the PicoCalc keyboard (STM32 at 0x1F, ~10 kHz) ----

#define KBD_SDA      CONFIG_LOADER_KBD_SDA_GPIO
#define KBD_SCL      CONFIG_LOADER_KBD_SCL_GPIO
#define KBD_ADDR     0x1F
#define KBD_REG_FIFO 0x09
#define KBD_KEY_UP   0xB5
#define KBD_KEY_F1   0x81
#define KBD_KEY_F5   0x85

static void od_pin_init(uint32_t pin)
{
    pin_input_pullup(pin);
    gpio_ll_od_enable(&GPIO, pin);
    gpio_ll_set_level(&GPIO, pin, 1);
    esp_rom_gpio_connect_out_signal(pin, SIG_GPIO_OUT_IDX, false, false);
    gpio_ll_output_enable(&GPIO, pin);
}

static inline void i2c_delay(void)
{
    esp_rom_delay_us(50);
}

static inline void sda_set(int level)
{
    gpio_ll_set_level(&GPIO, KBD_SDA, level);
}

static inline int sda_get(void)
{
    return gpio_ll_get_level(&GPIO, KBD_SDA);
}

static void scl_release(void)
{
    gpio_ll_set_level(&GPIO, KBD_SCL, 1);
    // allow clock stretching, up to ~5 ms
    for (int i = 0; i < 500 && !gpio_ll_get_level(&GPIO, KBD_SCL); i++) {
        esp_rom_delay_us(10);
    }
}

static inline void scl_low(void)
{
    gpio_ll_set_level(&GPIO, KBD_SCL, 0);
}

static void i2c_start(void)
{
    sda_set(1);
    scl_release();
    i2c_delay();
    sda_set(0);
    i2c_delay();
    scl_low();
    i2c_delay();
}

static void i2c_stop(void)
{
    sda_set(0);
    i2c_delay();
    scl_release();
    i2c_delay();
    sda_set(1);
    i2c_delay();
}

static bool i2c_write_byte(uint8_t b)
{
    for (int i = 7; i >= 0; i--) {
        sda_set((b >> i) & 1);
        i2c_delay();
        scl_release();
        i2c_delay();
        scl_low();
    }
    sda_set(1);
    i2c_delay();
    scl_release();
    i2c_delay();
    bool ack = sda_get() == 0;
    scl_low();
    i2c_delay();
    return ack;
}

static uint8_t i2c_read_byte(bool ack)
{
    uint8_t b = 0;
    sda_set(1);
    for (int i = 0; i < 8; i++) {
        i2c_delay();
        scl_release();
        i2c_delay();
        b = (uint8_t)((b << 1) | sda_get());
        scl_low();
    }
    sda_set(ack ? 0 : 1);
    i2c_delay();
    scl_release();
    i2c_delay();
    scl_low();
    sda_set(1);
    i2c_delay();
    return b;
}

// Pop one event from the keyboard FIFO. False if the keyboard did not answer.
static bool kbd_read_event(uint8_t *state, uint8_t *key)
{
    i2c_start();
    bool ok = i2c_write_byte(KBD_ADDR << 1) && i2c_write_byte(KBD_REG_FIFO);
    i2c_stop();
    if (!ok) {
        return false;
    }
    esp_rom_delay_us(2000); // the STM32 needs a turnaround before the read
    i2c_start();
    if (!i2c_write_byte((KBD_ADDR << 1) | 1)) {
        i2c_stop();
        return false;
    }
    *state = i2c_read_byte(true);
    *key = i2c_read_byte(false);
    i2c_stop();
    return true;
}
#endif // CONFIG_LOADER_KBD_ENABLE

// Watch the BOOT button and the PicoCalc keyboard for CONFIG_LOADER_TRIGGER_WINDOW_MS.
static bool loader_requested(void)
{
#if CONFIG_LOADER_BUTTON_GPIO >= 0
    pin_input_pullup(CONFIG_LOADER_BUTTON_GPIO);
    uint32_t low_since = 0;
    bool low = false;
#endif
#if CONFIG_LOADER_KBD_ENABLE
    od_pin_init(KBD_SDA);
    od_pin_init(KBD_SCL);
    uint32_t next_kbd = 0;
#endif
    esp_rom_delay_us(100); // let the pull-ups settle

    uint32_t start = esp_log_early_timestamp();
    for (;;) {
        uint32_t elapsed = esp_log_early_timestamp() - start;

#if CONFIG_LOADER_BUTTON_GPIO >= 0
        if (gpio_ll_get_level(&GPIO, CONFIG_LOADER_BUTTON_GPIO) == 0) {
            if (!low) {
                low = true;
                low_since = elapsed;
            } else if (elapsed - low_since >= CONFIG_LOADER_BUTTON_DEBOUNCE_MS) {
                return true;
            }
        } else {
            low = false;
        }
#endif

#if CONFIG_LOADER_KBD_ENABLE
        if (elapsed >= next_kbd) {
            uint8_t state, key;
            if (kbd_read_event(&state, &key) && (state == 1 || state == 2) &&
                (key == KBD_KEY_UP || key == KBD_KEY_F1 || key == KBD_KEY_F5)) {
                return true;
            }
            next_kbd = elapsed + 10;
        }
#endif

#if CONFIG_LOADER_BUTTON_GPIO >= 0
        if (low) {
            continue; // finish debouncing a press that started near the end of the window
        }
#endif
        if (elapsed >= CONFIG_LOADER_TRIGGER_WINDOW_MS) {
            return false;
        }
        esp_rom_delay_us(1000);
    }
}

#if CONFIG_LIBC_NEWLIB
// Return global reent struct if any newlib functions are linked to bootloader
struct _reent *__getreent(void)
{
    return _GLOBAL_REENT;
}
#endif
