// tusb_config.h - TinyUSB: one mass-storage interface (USB drive mode)
#pragma once

#define BOARD_TUD_RHPORT      0
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUSB_OS           OPT_OS_PICO
#define CFG_TUD_ENABLED       1
#define CFG_TUD_ENDPOINT0_SIZE 64

#define CFG_TUD_MSC     1
#define CFG_TUD_CDC     0
#define CFG_TUD_HID     0
#define CFG_TUD_MIDI    0
#define CFG_TUD_VENDOR  0

#define CFG_TUD_MSC_EP_BUFSIZE 4096
