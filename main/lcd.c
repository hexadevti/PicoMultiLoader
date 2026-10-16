#include "lcd.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_check.h"
#include "esp_log.h"
#include "hal/usb_serial_jtag_ll.h"
#include "uifont.h"

static const char *TAG = "lcd";

#define CHUNK_LINES 32

static esp_lcd_panel_io_handle_t s_io;
static uint16_t *s_fb; // byte-swapped RGB565 (big-endian on the wire)
static SemaphoreHandle_t s_done;

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

static bool on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_done, &woken);
    return woken == pdTRUE;
}

// GPIO24/25 come out of reset owned by the USB-Serial-JTAG PHY with its D+ pull-up on.
// Hand them to the GPIO matrix before using them as DC/RST.
static void release_usb_jtag_pads(void)
{
    USB_SERIAL_JTAG.conf0.pad_pull_override = 1;
    USB_SERIAL_JTAG.conf0.dp_pullup = 0;
    USB_SERIAL_JTAG.conf0.dm_pullup = 0;
    usb_serial_jtag_ll_phy_enable_pad(false);
}

static esp_err_t lcd_bus_init(void)
{
    release_usb_jtag_pads();

    gpio_config_t out = {
        .pin_bit_mask = (1ULL << LCD_CS_PIN) | (1ULL << LCD_RST_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&out), TAG, "gpio");
    gpio_set_level(LCD_CS_PIN, 0); // held low for good, as in emu8

    spi_bus_config_t bus = {
        .sclk_io_num = LCD_SCK_PIN,
        .mosi_io_num = LCD_MOSI_PIN,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * CHUNK_LINES * 2,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = LCD_DC_PIN,
        .cs_gpio_num = -1,
        .pclk_hz = LCD_SPI_HZ,
        .spi_mode = 0,
        .trans_queue_depth = 4,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .on_color_trans_done = on_color_done,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &s_io),
                        TAG, "panel io");

    s_done = xSemaphoreCreateBinary();
    s_fb = heap_caps_aligned_calloc(128, LCD_W * LCD_H, sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!s_fb) {
        ESP_LOGE(TAG, "No memory for the framebuffer");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t lcd_init(void)
{
    if (!s_io) {
        ESP_RETURN_ON_ERROR(lcd_bus_init(), TAG, "bus init");
    }

    gpio_set_level(LCD_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(LCD_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(LCD_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(200));

    for (size_t i = 0; i < sizeof(kInit);) {
        uint8_t cmd = kInit[i++];
        uint8_t n = kInit[i++];
        esp_lcd_panel_io_tx_param(s_io, cmd, n ? &kInit[i] : NULL, n);
        i += n;
    }
    esp_lcd_panel_io_tx_param(s_io, 0x11, NULL, 0); // sleep out
    vTaskDelay(pdMS_TO_TICKS(120));
    esp_lcd_panel_io_tx_param(s_io, 0x29, NULL, 0); // display on
    vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

void lcd_flush(void)
{
    if (!s_io) {
        return;
    }
    const uint8_t ca[4] = {0, 0, (LCD_W - 1) >> 8, (LCD_W - 1) & 0xFF};
    const uint8_t ra[4] = {0, 0, (LCD_H - 1) >> 8, (LCD_H - 1) & 0xFF};
    esp_lcd_panel_io_tx_param(s_io, 0x2A, ca, 4);
    esp_lcd_panel_io_tx_param(s_io, 0x2B, ra, 4);
    esp_lcd_panel_io_tx_color(s_io, 0x2C, s_fb, LCD_W * LCD_H * 2);
    xSemaphoreTake(s_done, portMAX_DELAY);
}

static inline uint16_t swap16(uint16_t c)
{
    return (uint16_t)((c << 8) | (c >> 8));
}

void lcd_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!s_fb) {
        return;
    }
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > LCD_W ? LCD_W : x + w, y1 = y + h > LCD_H ? LCD_H : y + h;
    uint16_t c = swap16(color);
    for (int j = y0; j < y1; j++) {
        uint16_t *row = &s_fb[j * LCD_W];
        for (int i = x0; i < x1; i++) {
            row[i] = c;
        }
    }
}

void lcd_hline(int x, int y, int w, uint16_t color)
{
    lcd_fill_rect(x, y, w, 1, color);
}

int lcd_font_width(lcd_font_t font)
{
    return font == FONT_SMALL ? 6 : 7;
}

int lcd_font_height(lcd_font_t font)
{
    return font == FONT_SMALL ? 10 : 13;
}

int lcd_text(int x, int y, const char *s, uint16_t color, lcd_font_t font)
{
    const uint8_t *glyphs = font == FONT_SMALL ? UIFONT_6X10 : UIFONT_7X13;
    const int fw = lcd_font_width(font), fh = lcd_font_height(font);
    uint16_t c = swap16(color);
    for (; *s; s++, x += fw) {
        uint8_t ch = (uint8_t)*s;
        if (ch < 0x20 || ch > 0x7E) {
            ch = '?';
        }
        const uint8_t *g = &glyphs[(ch - 0x20) * fh];
        for (int r = 0; r < fh; r++) {
            int py = y + r;
            if (py < 0 || py >= LCD_H || !s_fb) {
                continue;
            }
            for (int b = 0; b < 8; b++) {
                int px = x + b;
                if ((g[r] & (0x80 >> b)) && px >= 0 && px < LCD_W) {
                    s_fb[py * LCD_W + px] = c;
                }
            }
        }
    }
    return x;
}

void lcd_dump(void)
{
    if (!s_fb) {
        return;
    }
    // One line per row: "FB <y> " then runs of "<rgb565 hex>" or "<rgb565 hex>*<count>".
    for (int y = 0; y < LCD_H; y++) {
        const uint16_t *row = &s_fb[y * LCD_W];
        printf("FB %d ", y);
        for (int x = 0; x < LCD_W;) {
            int n = 1;
            while (x + n < LCD_W && row[x + n] == row[x]) {
                n++;
            }
            if (n > 1) {
                printf("%04x*%d,", swap16(row[x]), n);
            } else {
                printf("%04x,", swap16(row[x]));
            }
            x += n;
        }
        printf("\n");
        if (y % 16 == 15) {
            fflush(stdout);
            vTaskDelay(1); // let the idle task feed the watchdog
        }
    }
    printf("FB end\n");
    fflush(stdout);
}
