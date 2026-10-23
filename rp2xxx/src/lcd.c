#include "lcd.h"

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "uifont.h"

#define CHUNK_ROWS 1 // each chunk buffer is RAM the RP2350 heap would otherwise get

static uint8_t s_fb[LCD_W * LCD_H / 2]; // two pixels per byte: low nibble = even x
static uint16_t s_palette[16];
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

void lcd_set_palette(lcd_color_t index, uint16_t rgb565)
{
    s_palette[index & 15] = rgb565;
}

static void expand_rows(uint16_t *out, int y, int rows)
{
    const uint8_t *src = &s_fb[y * (LCD_W / 2)];
    for (int i = 0; i < rows * LCD_W / 2; i++) {
        uint8_t b = src[i];
        *out++ = s_palette[b & 15];
        *out++ = s_palette[b >> 4];
    }
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
        expand_rows(s_chunk[buf], row, n);
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

void lcd_flush(void)
{
    lcd_flush_rows(0, LCD_H);
}

static inline void put_pixel(int x, int y, lcd_color_t c)
{
    uint8_t *p = &s_fb[y * (LCD_W / 2) + (x >> 1)];
    *p = (x & 1) ? (uint8_t)((*p & 0x0F) | (c << 4)) : (uint8_t)((*p & 0xF0) | (c & 15));
}

void lcd_fill_rect(int x, int y, int w, int h, lcd_color_t color)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > LCD_W ? LCD_W : x + w, y1 = y + h > LCD_H ? LCD_H : y + h;
    if (x0 >= x1) {
        return;
    }
    uint8_t both = (uint8_t)((color & 15) * 0x11);
    for (int j = y0; j < y1; j++) {
        int i = x0;
        if (i & 1) {
            put_pixel(i++, j, color);
        }
        int full = (x1 - i) / 2;
        memset(&s_fb[j * (LCD_W / 2) + i / 2], both, full);
        i += 2 * full;
        if (i < x1) {
            put_pixel(i, j, color);
        }
    }
}

void lcd_hline(int x, int y, int w, lcd_color_t color)
{
    lcd_fill_rect(x, y, w, 1, color);
}

void lcd_checker(int y, int h, lcd_color_t color)
{
    uint8_t c = color & 15;
    for (int j = y < 0 ? 0 : y; j < y + h && j < LCD_H; j++) {
        // even rows: even pixels (low nibbles); odd rows: odd pixels (high nibbles)
        uint8_t keep = (j & 1) ? 0x0F : 0xF0, set = (j & 1) ? (uint8_t)(c << 4) : c;
        uint8_t *p = &s_fb[j * (LCD_W / 2)];
        for (int i = 0; i < LCD_W / 2; i++) {
            p[i] = (uint8_t)((p[i] & keep) | set);
        }
    }
}

int lcd_font_width(lcd_font_t font)
{
    return font == FONT_SMALL ? 6 : 7;
}

int lcd_font_height(lcd_font_t font)
{
    return font == FONT_SMALL ? 10 : 13;
}

int lcd_text(int x, int y, const char *s, lcd_color_t color, lcd_font_t font)
{
    const uint8_t *glyphs = font == FONT_SMALL ? UIFONT_6X10 : UIFONT_7X13;
    const int fw = lcd_font_width(font), fh = lcd_font_height(font);
    for (; *s; s++, x += fw) {
        uint8_t ch = (uint8_t)*s;
        if (ch < 0x20 || ch > 0x7E) {
            ch = '?';
        }
        const uint8_t *g = &glyphs[(ch - 0x20) * fh];
        for (int r = 0; r < fh; r++) {
            int py = y + r;
            if (py < 0 || py >= LCD_H) {
                continue;
            }
            for (int b = 0; b < 8; b++) {
                int px = x + b;
                if ((g[r] & (0x80 >> b)) && px >= 0 && px < LCD_W) {
                    put_pixel(px, py, color);
                }
            }
        }
    }
    return x;
}

static inline uint16_t pixel_rgb(int x, int y)
{
    uint8_t b = s_fb[y * (LCD_W / 2) + (x >> 1)];
    return s_palette[(x & 1) ? b >> 4 : b & 15];
}

void lcd_dump(void)
{
    // One line per row: "FB <y> " then runs of "<rgb565 hex>" or "<rgb565 hex>*<count>".
    for (int y = 0; y < LCD_H; y++) {
        printf("FB %d ", y);
        for (int x = 0; x < LCD_W;) {
            uint16_t c = pixel_rgb(x, y);
            int n = 1;
            while (x + n < LCD_W && pixel_rgb(x + n, y) == c) {
                n++;
            }
            if (n > 1) {
                printf("%04x*%d,", c, n);
            } else {
                printf("%04x,", c);
            }
            x += n;
        }
        printf("\n");
    }
    printf("FB end\n");
    fflush(stdout);
}
