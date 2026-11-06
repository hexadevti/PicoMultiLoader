// port_rp2350.c - picocalc.h on a Raspberry Pi Pico 2 / Pico 2 W in the PicoCalc (Pico SDK).
//
//   screen    ST7365P 320x320 on SPI1: SCK 10, MOSI 11, MISO 12, CS 13, DC 14, RST 15
//   keyboard  STM32 at 0x1F on I2C1: SDA 6, SCL 7, 10 kHz
//   console   UART0 on GPIO 0/1, 115200 baud (the PicoCalc's USB-C port)
#include "picocalc.h"

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/spi.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"

#define LCD_SPI      spi1
#define LCD_SCK_PIN  10
#define LCD_MOSI_PIN 11
#define LCD_MISO_PIN 12
#define LCD_CS_PIN   13
#define LCD_DC_PIN   14
#define LCD_RST_PIN  15
#define LCD_SPI_HZ   (50 * 1000 * 1000) // the SDK picks the nearest rate the clock allows

#define KBD_I2C      i2c1
#define KBD_SDA_PIN  6
#define KBD_SCL_PIN  7
#define KBD_ADDR     0x1F
#define KBD_I2C_HZ   10000 // the keyboard firmware drops bytes at faster clocks
#define KBD_TIMEOUT_US 50000

static int s_dma = -1;

// ST7365P init sequence: {command, number of arguments, arguments...}
static const uint8_t kInit[] = {
    0xE0, 15, 0x00, 0x03, 0x09, 0x08, 0x16, 0x0A, 0x3F, 0x78, 0x4C, 0x09, 0x0A, 0x08, 0x16, 0x1A, 0x0F,
    0xE1, 15, 0x00, 0x16, 0x19, 0x03, 0x0F, 0x05, 0x32, 0x45, 0x46, 0x04, 0x0E, 0x0D, 0x35, 0x37, 0x0F,
    0xC0, 2, 0x17, 0x15,
    0xC1, 1, 0x41,
    0xC5, 3, 0x00, 0x12, 0x80,
    0x36, 1, 0x48, // MADCTL: MX | BGR
    0x3A, 1, 0x55, // 16 bits per pixel (RGB565)
    0xB0, 1, 0x00,
    0xB1, 1, 0xA0,
    0x21, 0,       // inversion on
    0xB4, 1, 0x02,
    0xB6, 3, 0x02, 0x02, 0x3B,
    0xB7, 1, 0xC6,
    0xE9, 1, 0x00,
    0xF7, 4, 0xA9, 0x51, 0x2C, 0x82,
};

static void lcd_cmd(uint8_t cmd, const uint8_t *args, size_t n)
{
    gpio_put(LCD_CS_PIN, 0);
    gpio_put(LCD_DC_PIN, 0);
    spi_write_blocking(LCD_SPI, &cmd, 1);
    gpio_put(LCD_DC_PIN, 1);
    if (n) {
        spi_write_blocking(LCD_SPI, args, n);
    }
    gpio_put(LCD_CS_PIN, 1);
}

bool port_lcd_init(void)
{
    stdio_init_all(); // printf() goes to UART0

    spi_init(LCD_SPI, LCD_SPI_HZ);
    spi_set_format(LCD_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(LCD_SCK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(LCD_MOSI_PIN, GPIO_FUNC_SPI);
    gpio_set_function(LCD_MISO_PIN, GPIO_FUNC_SPI);
    const uint pins[] = {LCD_CS_PIN, LCD_DC_PIN, LCD_RST_PIN};
    for (int i = 0; i < 3; i++) {
        gpio_init(pins[i]);
        gpio_set_dir(pins[i], GPIO_OUT);
        gpio_put(pins[i], 1);
    }

    // DMA: 16-bit pixels from the frame buffer to the SPI data register
    s_dma = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(s_dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, spi_get_dreq(LCD_SPI, true));
    dma_channel_configure(s_dma, &c, &spi_get_hw(LCD_SPI)->dr, NULL, 0, false);

    gpio_put(LCD_RST_PIN, 0);
    sleep_ms(10);
    gpio_put(LCD_RST_PIN, 1);
    sleep_ms(200);
    for (size_t i = 0; i < sizeof(kInit);) {
        uint8_t cmd = kInit[i++];
        uint8_t n = kInit[i++];
        lcd_cmd(cmd, &kInit[i], n);
        i += n;
    }
    lcd_cmd(0x11, NULL, 0); // sleep out
    sleep_ms(120);
    lcd_cmd(0x29, NULL, 0); // display on
    return true;
}

void port_lcd_send(const uint16_t *fb)
{
    const uint8_t ca[4] = {0, 0, (PC_W - 1) >> 8, (PC_W - 1) & 0xFF};
    const uint8_t ra[4] = {0, 0, (PC_H - 1) >> 8, (PC_H - 1) & 0xFF};
    lcd_cmd(0x2A, ca, 4); // columns
    lcd_cmd(0x2B, ra, 4); // rows

    gpio_put(LCD_CS_PIN, 0);
    gpio_put(LCD_DC_PIN, 0);
    uint8_t ramwr = 0x2C;
    spi_write_blocking(LCD_SPI, &ramwr, 1);
    gpio_put(LCD_DC_PIN, 1);

    // 16-bit SPI frames send each RGB565 pixel high byte first, as the panel wants
    spi_set_format(LCD_SPI, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    dma_channel_transfer_from_buffer_now(s_dma, fb, PC_W * PC_H);
    dma_channel_wait_for_finish_blocking(s_dma);
    while (spi_is_busy(LCD_SPI)) {
    }
    while (spi_is_readable(LCD_SPI)) { // throw away what came in while DMA wrote
        (void)spi_get_hw(LCD_SPI)->dr;
    }
    spi_get_hw(LCD_SPI)->icr = SPI_SSPICR_RORIC_BITS;
    spi_set_format(LCD_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_put(LCD_CS_PIN, 1);
}

void port_kbd_init(void)
{
    i2c_init(KBD_I2C, KBD_I2C_HZ);
    gpio_set_function(KBD_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(KBD_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(KBD_SDA_PIN);
    gpio_pull_up(KBD_SCL_PIN);
}

bool port_kbd_read(uint8_t reg, uint8_t out[2])
{
    if (i2c_write_timeout_us(KBD_I2C, KBD_ADDR, &reg, 1, false, KBD_TIMEOUT_US) != 1) {
        return false;
    }
    sleep_us(2000); // the STM32 needs a moment before the read
    return i2c_read_timeout_us(KBD_I2C, KBD_ADDR, out, 2, false, KBD_TIMEOUT_US) == 2;
}

bool port_kbd_write(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = {reg | 0x80, value};
    return i2c_write_timeout_us(KBD_I2C, KBD_ADDR, buf, 2, false, KBD_TIMEOUT_US) == 2;
}

uint64_t port_time_us(void)
{
    return time_us_64();
}

void port_sleep_ms(uint32_t ms)
{
    sleep_ms(ms);
}

const char *port_board_name(void)
{
    return "RP2350";
}

void port_exit_to_menu(void)
{
    // The same request as uf2loader: PicoMultiLoader opens its menu after this reset.
    watchdog_hw->scratch[0] = 0xe98cc638;
    watchdog_hw->scratch[1] = 1;
    watchdog_reboot(0, 0, 0);
    for (;;) {
        tight_loop_contents();
    }
}
