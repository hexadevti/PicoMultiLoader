// lcd_bus.c - the ESP32-P4 side of the panel: SPI2 with DMA through esp_lcd (the drawing is
// common/pml/lcd.c)
#include "lcd.h"

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "hal/usb_serial_jtag_ll.h"

static const char *TAG = "lcd";

#define CHUNK_ROWS 16
#define QUEUE      2 // chunk buffers, one filling while the other is sent

static esp_lcd_panel_io_handle_t s_io;
static uint16_t *s_chunk[QUEUE]; // byte-swapped RGB565 (big-endian on the wire)
static SemaphoreHandle_t s_free;  // chunk buffers not being sent

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
    xSemaphoreGiveFromISR(s_free, &woken);
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

static bool lcd_bus_init(void)
{
    release_usb_jtag_pads();

    gpio_config_t out = {
        .pin_bit_mask = (1ULL << LCD_CS_PIN) | (1ULL << LCD_RST_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    if (gpio_config(&out) != ESP_OK) {
        return false;
    }
    gpio_set_level(LCD_CS_PIN, 0); // held low for good, as in emu8

    spi_bus_config_t bus = {
        .sclk_io_num = LCD_SCK_PIN,
        .mosi_io_num = LCD_MOSI_PIN,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * CHUNK_ROWS * 2,
    };
    if (spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed");
        return false;
    }

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = LCD_DC_PIN,
        .cs_gpio_num = -1,
        .pclk_hz = LCD_SPI_HZ,
        .spi_mode = 0,
        .trans_queue_depth = QUEUE + 2,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .on_color_trans_done = on_color_done,
    };
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &s_io) != ESP_OK) {
        ESP_LOGE(TAG, "Panel IO init failed");
        s_io = NULL;
        return false;
    }

    s_free = xSemaphoreCreateCounting(QUEUE, QUEUE);
    for (int i = 0; i < QUEUE; i++) {
        s_chunk[i] = heap_caps_aligned_alloc(64, LCD_W * CHUNK_ROWS * sizeof(uint16_t), MALLOC_CAP_DMA);
        if (!s_chunk[i]) {
            ESP_LOGE(TAG, "No memory for the line buffers");
            return false;
        }
    }
    return true;
}

bool lcd_init(void)
{
    if (!s_io && !lcd_bus_init()) {
        return false;
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
    return true;
}

void lcd_flush_rows(int y, int h)
{
    if (!s_io || !s_chunk[QUEUE - 1]) {
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
    esp_lcd_panel_io_tx_param(s_io, 0x2A, ca, 4);
    esp_lcd_panel_io_tx_param(s_io, 0x2B, ra, 4);

    // Expand the next rows while DMA sends the previous ones. The first chunk starts the RAM
    // write (0x2C); the rest continue it (no command).
    int buf = 0;
    for (int row = y; row < y + h; row += CHUNK_ROWS) {
        int n = y + h - row < CHUNK_ROWS ? y + h - row : CHUNK_ROWS;
        xSemaphoreTake(s_free, portMAX_DELAY);
        uint16_t *px = s_chunk[buf];
        lcd_expand_rows(px, row, n);
        for (int i = 0; i < n * LCD_W; i++) {
            px[i] = (uint16_t)(px[i] << 8 | px[i] >> 8);
        }
        esp_lcd_panel_io_tx_color(s_io, row == y ? 0x2C : -1, px, (size_t)n * LCD_W * 2);
        buf = (buf + 1) % QUEUE;
    }
    for (int i = 0; i < QUEUE; i++) { // all sent: the buffers are free again
        xSemaphoreTake(s_free, portMAX_DELAY);
    }
    for (int i = 0; i < QUEUE; i++) {
        xSemaphoreGive(s_free);
    }
}
