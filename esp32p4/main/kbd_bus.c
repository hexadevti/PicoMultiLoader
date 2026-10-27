// kbd_bus.c - the ESP32-P4 side of the keyboard: I2C (the key logic is common/pml/keyboard.c)
#include "keyboard.h"

#include "driver/i2c_master.h"
#include "esp_rom_sys.h"
#include "board.h"

#define I2C_TIMEOUT_MS 50

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;

void kbd_bus_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = KBD_SDA_PIN,
        .scl_io_num = KBD_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_cfg, &s_bus) != ESP_OK) {
        s_bus = NULL;
        return;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = KBD_I2C_ADDR,
        .scl_speed_hz = KBD_I2C_HZ,
    };
    if (i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev) != ESP_OK) {
        s_dev = NULL;
    }
}

bool kbd_bus_read(uint8_t reg, uint8_t out[2])
{
    if (!s_dev || i2c_master_transmit(s_dev, &reg, 1, I2C_TIMEOUT_MS) != ESP_OK) {
        return false;
    }
    esp_rom_delay_us(2000); // the STM32 needs a turnaround before the read
    return i2c_master_receive(s_dev, out, 2, I2C_TIMEOUT_MS) == ESP_OK;
}

bool kbd_bus_write(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = {reg | 0x80, value};
    return s_dev && i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS) == ESP_OK;
}
