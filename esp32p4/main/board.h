// board.h - ClockworkPi PicoCalc mainboard with a Waveshare ESP32-P4-WIFI6 in the Pico socket.
// Pin map from emu8 (board.h BOARD_PICOCALC_P4, verified by tools/picocalc-p4-bringup).
#pragma once

// --- display: ST7365P/ILI9488 320x320 on SPI2. CS is held low for good.
//     DC/RST are GPIO24/25, the USB-Serial-JTAG pads: lcd_init() releases them first. ---
#define LCD_SPI_HOST    SPI2_HOST
#define LCD_SCK_PIN     3
#define LCD_MOSI_PIN    2
#define LCD_CS_PIN      7
#define LCD_DC_PIN      24
#define LCD_RST_PIN     25
#define LCD_W           320
#define LCD_H           320
#define LCD_SPI_HZ      (40 * 1000 * 1000)

// --- keyboard: STM32F103 at 0x1F, 10 kHz (its firmware drops bytes at faster clocks) ---
#define KBD_SDA_PIN     50
#define KBD_SCL_PIN     49
#define KBD_I2C_ADDR    0x1F
#define KBD_I2C_HZ      10000

// --- microSD on SPI3. GPIO39-48 are powered by the P4's internal LDO channel 4, which must be
//     on at 3.3 V before 46/47/48 work. ---
#define SD_SPI_HOST     SPI3_HOST
#define SD_SCK_PIN      46
#define SD_MISO_PIN     48
#define SD_MOSI_PIN     33
#define SD_CS_PIN       47
#define SD_IO_LDO_CHAN  4
#define SD_SPI_KHZ      20000
