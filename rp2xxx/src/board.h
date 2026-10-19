// board.h - ClockworkPi PicoCalc mainboard with a Raspberry Pi Pico / Pico W (RP2040) or
// Pico 2 / Pico 2 W (RP2350) in the Pico socket.
#pragma once

// --- display: ST7365P 320x320 on SPI1 ---
#define LCD_SPI         spi1
#define LCD_SCK_PIN     10
#define LCD_MOSI_PIN    11
#define LCD_MISO_PIN    12
#define LCD_CS_PIN      13
#define LCD_DC_PIN      14
#define LCD_RST_PIN     15
#define LCD_W           320
#define LCD_H           320
#define LCD_SPI_HZ      (50 * 1000 * 1000) // the SDK picks the nearest rate the clock allows

// --- keyboard: STM32F103 at 0x1F on I2C1, 10 kHz (its firmware drops bytes at faster clocks) ---
#define KBD_I2C         i2c1
#define KBD_SDA_PIN     6
#define KBD_SCL_PIN     7
#define KBD_I2C_ADDR    0x1F
#define KBD_I2C_HZ      10000

// --- microSD on SPI0, card-detect switch to GND (low = card in) ---
#define SD_SPI          spi0
#define SD_SCK_PIN      18
#define SD_MOSI_PIN     19
#define SD_MISO_PIN     16
#define SD_CS_PIN       17
#define SD_DET_PIN      22
#define SD_SPI_HZ       (25 * 1000 * 1000)

// --- serial console on the PicoCalc's USB-C port (through its CH340) ---
#define CONSOLE_UART    uart0
#define CONSOLE_TX_PIN  0
#define CONSOLE_RX_PIN  1
#define CONSOLE_BAUD    115200
