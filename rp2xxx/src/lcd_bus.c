// lcd_bus.c - the RP2040/RP2350 side of the panel: SPI with DMA (the drawing is common/pml/lcd.c)
#include "lcd.h"

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"

#define CHUNK_ROWS 1 // each chunk buffer is RAM the RP2350 heap would otherwise get

static uint16_t s_chunk[2][LCD_W * CHUNK_ROWS];
static int s_dma = -1;

// ST7365P init sequence, from emu8's display_picocalc.cpp: {cmd, nargs, args...}
static const uint8_t kInit[] = {
    0xE0, 15, 0x00, 0x03, 0x09, 0x08, 0x16, 0x0A, 0x3F, 0x78, 0x4C, 0x09, 0x0A, 0x08, 0x16, 0x1A, 0x0F,
    0xE1, 15, 0x00, 0x16, 0x19, 0x03, 0x0F, 0x05, 0x32, 0x45, 0x46, 0x04, 0x0E, 0x0D, 0x35, 0x37, 0x0F,
    0xC0, 2, 0x17, 0x15,
    0xC1, 1, 0x41,
    0xC5, 3, 0x00, 0x12, 0x80,
    0x36, 1, 0x48, // MADCTL: MX | BGR
    0x3A, 1, 0x55, // RGB565
    0xB0, 1, 0x00,
    0xB1, 1, 0xA0,
    0x21, 0,       // INVON
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

static void lcd_bus_init(void)
{
    spi_init(LCD_SPI, LCD_SPI_HZ);
    spi_set_format(LCD_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(LCD_SCK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(LCD_MOSI_PIN, GPIO_FUNC_SPI);
    gpio_set_function(LCD_MISO_PIN, GPIO_FUNC_SPI);

    const uint pins[] = {LCD_CS_PIN, LCD_DC_PIN, LCD_RST_PIN};
    for (size_t i = 0; i < 3; i++) {
        gpio_init(pins[i]);
        gpio_set_dir(pins[i], GPIO_OUT);
        gpio_put(pins[i], 1);
    }

    s_dma = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(s_dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, spi_get_dreq(LCD_SPI, true));
    dma_channel_configure(s_dma, &c, &spi_get_hw(LCD_SPI)->dr, NULL, 0, false);
}

bool lcd_init(void)
{
    if (s_dma < 0) {
        lcd_bus_init();
    }
    gpio_put(LCD_RST_PIN, 1);
    sleep_ms(10);
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
    sleep_ms(20);
    return true;
}

void lcd_flush_rows(int y, int h)
{
    if (s_dma < 0) {
        return;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (y + h > LCD_H) {
        h = LCD_H - y;
    }
    if (h <= 0) {
        return;
    }
    const uint8_t ca[4] = {0, 0, (LCD_W - 1) >> 8, (LCD_W - 1) & 0xFF};
    const uint8_t ra[4] = {y >> 8, y & 0xFF, (y + h - 1) >> 8, (y + h - 1) & 0xFF};
    lcd_cmd(0x2A, ca, 4);
    lcd_cmd(0x2B, ra, 4);

    gpio_put(LCD_CS_PIN, 0);
    gpio_put(LCD_DC_PIN, 0);
    uint8_t ramwr = 0x2C;
    spi_write_blocking(LCD_SPI, &ramwr, 1);
    gpio_put(LCD_DC_PIN, 1);
    spi_set_format(LCD_SPI, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    // Expand the next rows while DMA sends the previous ones.
    int buf = 0;
    for (int row = y; row < y + h; row += CHUNK_ROWS) {
        int n = y + h - row < CHUNK_ROWS ? y + h - row : CHUNK_ROWS;
        lcd_expand_rows(s_chunk[buf], row, n);
        dma_channel_wait_for_finish_blocking(s_dma);
        dma_channel_transfer_from_buffer_now(s_dma, s_chunk[buf], n * LCD_W);
        buf ^= 1;
    }
    dma_channel_wait_for_finish_blocking(s_dma);
    while (spi_is_busy(LCD_SPI)) {
    }
    while (spi_is_readable(LCD_SPI)) { // nobody reads while DMA writes
        (void)spi_get_hw(LCD_SPI)->dr;
    }
    spi_get_hw(LCD_SPI)->icr = SPI_SSPICR_RORIC_BITS;
    spi_set_format(LCD_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_put(LCD_CS_PIN, 1);
}

