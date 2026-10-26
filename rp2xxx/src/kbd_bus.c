// kbd_bus.c - the RP2040/RP2350 side of the keyboard: I2C (the key logic is common/pml/keyboard.c)
#include "keyboard.h"

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "board.h"

#define I2C_TIMEOUT_US 50000

void kbd_bus_init(void)
{
    i2c_init(KBD_I2C, KBD_I2C_HZ);
    gpio_set_function(KBD_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(KBD_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(KBD_SDA_PIN);
    gpio_pull_up(KBD_SCL_PIN);
}

bool kbd_bus_read(uint8_t reg, uint8_t out[2])
{
    if (i2c_write_timeout_us(KBD_I2C, KBD_I2C_ADDR, &reg, 1, false, I2C_TIMEOUT_US) != 1) {
        return false;
    }
    sleep_us(2000); // the STM32 needs a turnaround before the read
    return i2c_read_timeout_us(KBD_I2C, KBD_I2C_ADDR, out, 2, false, I2C_TIMEOUT_US) == 2;
}

bool kbd_bus_write(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = {reg | 0x80, value};
    return i2c_write_timeout_us(KBD_I2C, KBD_I2C_ADDR, buf, sizeof(buf), false, I2C_TIMEOUT_US) == 2;
}
