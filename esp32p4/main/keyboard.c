#include "keyboard.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "board.h"

#define REG_FIFO  0x09 // key event FIFO: {state, key}
#define REG_BK2   0x0A // LCD backlight
#define REG_BAT   0x0B // battery: bits 0..6 percent, bit 7 charging

#define STATE_PRESSED  1
#define STATE_HOLD     2
#define STATE_RELEASED 3

#define REPEAT_DELAY_US  400000
#define REPEAT_PERIOD_US 70000

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static bool s_present;
static int s_failures;
static int s_held_key;
static int64_t s_next_repeat;

// Treat the keyboard as gone (PicoCalc switched off) only after a few failures in a row.
static void note_result(esp_err_t err)
{
    if (err == ESP_OK) {
        s_failures = 0;
        s_present = true;
    } else if (++s_failures >= 3) {
        s_present = false;
    }
}

static esp_err_t kbd_read(uint8_t reg, uint8_t out[2])
{
    esp_err_t err = i2c_master_transmit(s_dev, &reg, 1, 50);
    if (err == ESP_OK) {
        esp_rom_delay_us(2000); // the STM32 needs a turnaround before the read
        err = i2c_master_receive(s_dev, out, 2, 50);
    }
    note_result(err);
    return err;
}

static void kbd_write(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = {reg | 0x80, value};
    note_result(i2c_master_transmit(s_dev, buf, sizeof(buf), 50));
}

esp_err_t kbd_init(void)
{
    if (!s_bus) {
        i2c_master_bus_config_t bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = KBD_SDA_PIN,
            .scl_io_num = KBD_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_bus));
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = KBD_I2C_ADDR,
            .scl_speed_hz = KBD_I2C_HZ,
        };
        ESP_ERROR_CHECK(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev));
    }
    s_present = i2c_master_probe(s_bus, KBD_I2C_ADDR, 50) == ESP_OK;
    s_failures = 0;
    if (s_present) {
        // Drain events queued before we started (e.g. the key that opened the loader)
        uint8_t r[2];
        for (int i = 0; i < 32 && kbd_read(REG_FIFO, r) == ESP_OK && r[1] != 0; i++) {
        }
    }
    return s_present ? ESP_OK : ESP_ERR_NOT_FOUND;
}

bool kbd_present(void)
{
    return s_present;
}

static bool is_repeatable(int key)
{
    return key == KEY_UP || key == KEY_DOWN || key == KEY_PAGE_UP || key == KEY_PAGE_DOWN;
}

int kbd_poll(void)
{
    if (!s_dev) {
        return KEY_NONE;
    }
    uint8_t r[2];
    if (kbd_read(REG_FIFO, r) == ESP_OK && r[1] != 0) {
        int key = r[1];
        if (r[0] == STATE_PRESSED) {
            s_held_key = is_repeatable(key) ? key : KEY_NONE;
            s_next_repeat = esp_timer_get_time() + REPEAT_DELAY_US;
            return key;
        }
        if (r[0] == STATE_RELEASED && key == s_held_key) {
            s_held_key = KEY_NONE;
        }
        return KEY_NONE;
    }
    if (s_held_key != KEY_NONE && esp_timer_get_time() >= s_next_repeat) {
        s_next_repeat = esp_timer_get_time() + REPEAT_PERIOD_US;
        return s_held_key;
    }
    return KEY_NONE;
}

int kbd_battery(bool *charging)
{
    uint8_t r[2];
    if (!s_dev || kbd_read(REG_BAT, r) != ESP_OK) {
        return -1;
    }
    if (charging) {
        *charging = (r[1] & 0x80) != 0;
    }
    int pct = r[1] & 0x7F;
    return pct > 100 ? 100 : pct;
}

void kbd_set_lcd_backlight(uint8_t level)
{
    if (s_dev) {
        kbd_write(REG_BK2, level);
    }
}
