// port_esp32p4.c - picocalc.h on a Waveshare ESP32-P4-WIFI6 in the PicoCalc (ESP-IDF 5.5).
//
//   screen    ST7365P 320x320 on SPI2: SCK 3, MOSI 2, CS 7 (held low), DC 24, RST 25
//   keyboard  STM32 at 0x1F on I2C0: SDA 50, SCL 49, 10 kHz
//   console   UART0, 115200 baud
//
// GPIO24/25 are also the USB-Serial-JTAG pads, so the console must not use USB-Serial-JTAG
// (sdkconfig.defaults: CONFIG_ESP_CONSOLE_SECONDARY_NONE).
#include "picocalc.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"
#include "loader_api.h"

static const char *TAG = "picocalc";

#define LCD_SPI_HOST SPI2_HOST
#define LCD_SCK_PIN  3
#define LCD_MOSI_PIN 2
#define LCD_CS_PIN   7
#define LCD_DC_PIN   24
#define LCD_RST_PIN  25
#define LCD_SPI_HZ   (40 * 1000 * 1000)

#define KBD_SDA_PIN  50
#define KBD_SCL_PIN  49
#define KBD_ADDR     0x1F
#define KBD_I2C_HZ   10000 // the keyboard firmware drops bytes at faster clocks
#define KBD_TIMEOUT_MS 50

// The frame goes out in chunks of rows: one chunk is filled (and byte-swapped: the panel wants
// the high byte first) while DMA sends the other.
#define CHUNK_ROWS 16
#define QUEUE      2

static esp_lcd_panel_io_handle_t s_io;
static uint16_t *s_chunk[QUEUE];
static SemaphoreHandle_t s_free; // chunk buffers not being sent

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_kbd;

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

static bool on_chunk_sent(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_free, &woken);
    return woken == pdTRUE;
}

// GPIO24/25 come out of reset owned by the USB-Serial-JTAG PHY, with its D+ pull-up on.
// Give them to the GPIO matrix before using them as DC/RST.
static void release_usb_jtag_pads(void)
{
    USB_SERIAL_JTAG.conf0.pad_pull_override = 1;
    USB_SERIAL_JTAG.conf0.dp_pullup = 0;
    USB_SERIAL_JTAG.conf0.dm_pullup = 0;
    usb_serial_jtag_ll_phy_enable_pad(false);
}

bool port_lcd_init(void)
{
    release_usb_jtag_pads();

    gpio_config_t out = {
        .pin_bit_mask = (1ULL << LCD_CS_PIN) | (1ULL << LCD_RST_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&out));
    gpio_set_level(LCD_CS_PIN, 0); // the panel is the only device on this bus

    spi_bus_config_t bus = {
        .sclk_io_num = LCD_SCK_PIN,
        .mosi_io_num = LCD_MOSI_PIN,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = PC_W * CHUNK_ROWS * 2,
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
        .on_color_trans_done = on_chunk_sent,
    };
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &s_io) != ESP_OK) {
        ESP_LOGE(TAG, "panel IO init failed");
        return false;
    }
    s_free = xSemaphoreCreateCounting(QUEUE, QUEUE);
    for (int i = 0; i < QUEUE; i++) {
        s_chunk[i] = heap_caps_aligned_alloc(64, PC_W * CHUNK_ROWS * sizeof(uint16_t), MALLOC_CAP_DMA);
        if (!s_chunk[i]) {
            ESP_LOGE(TAG, "no memory for the DMA buffers");
            return false;
        }
    }

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
    return true;
}

void port_lcd_send(const uint16_t *fb)
{
    const uint8_t ca[4] = {0, 0, (PC_W - 1) >> 8, (PC_W - 1) & 0xFF};
    const uint8_t ra[4] = {0, 0, (PC_H - 1) >> 8, (PC_H - 1) & 0xFF};
    esp_lcd_panel_io_tx_param(s_io, 0x2A, ca, 4); // columns
    esp_lcd_panel_io_tx_param(s_io, 0x2B, ra, 4); // rows

    // The first chunk starts the RAM write (0x2C); the others continue it (no command).
    int buf = 0;
    for (int row = 0; row < PC_H; row += CHUNK_ROWS) {
        int n = PC_H - row < CHUNK_ROWS ? PC_H - row : CHUNK_ROWS;
        xSemaphoreTake(s_free, portMAX_DELAY);
        uint16_t *dst = s_chunk[buf];
        const uint16_t *src = &fb[row * PC_W];
        for (int i = 0; i < n * PC_W; i++) {
            dst[i] = (uint16_t)(src[i] << 8 | src[i] >> 8);
        }
        esp_lcd_panel_io_tx_color(s_io, row == 0 ? 0x2C : -1, dst, (size_t)n * PC_W * 2);
        buf = (buf + 1) % QUEUE;
    }
    for (int i = 0; i < QUEUE; i++) { // wait until everything is sent
        xSemaphoreTake(s_free, portMAX_DELAY);
    }
    for (int i = 0; i < QUEUE; i++) {
        xSemaphoreGive(s_free);
    }
}

void port_kbd_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = KBD_SDA_PIN,
        .scl_io_num = KBD_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_cfg, &s_i2c) != ESP_OK) {
        ESP_LOGE(TAG, "I2C init failed");
        return;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = KBD_ADDR,
        .scl_speed_hz = KBD_I2C_HZ,
    };
    if (i2c_master_bus_add_device(s_i2c, &dev_cfg, &s_kbd) != ESP_OK) {
        s_kbd = NULL;
    }
}

bool port_kbd_read(uint8_t reg, uint8_t out[2])
{
    if (!s_kbd || i2c_master_transmit(s_kbd, &reg, 1, KBD_TIMEOUT_MS) != ESP_OK) {
        return false;
    }
    esp_rom_delay_us(2000); // the STM32 needs a moment before the read
    return i2c_master_receive(s_kbd, out, 2, KBD_TIMEOUT_MS) == ESP_OK;
}

bool port_kbd_write(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = {reg | 0x80, value};
    return s_kbd && i2c_master_transmit(s_kbd, buf, 2, KBD_TIMEOUT_MS) == ESP_OK;
}

uint64_t port_time_us(void)
{
    return (uint64_t)esp_timer_get_time();
}

void port_sleep_ms(uint32_t ms)
{
    TickType_t t = pdMS_TO_TICKS(ms);
    vTaskDelay(t ? t : 1);
}

const char *port_board_name(void)
{
    return "ESP32-P4";
}

void port_exit_to_menu(void)
{
    picomultiloader_reboot_to_menu(); // without PicoMultiLoader: a plain restart
}
