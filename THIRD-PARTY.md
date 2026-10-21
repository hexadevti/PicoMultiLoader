# Third-party material

Everything in this repository is this project's own work, with the exceptions below. Each file
taken from elsewhere says where it came from in its header.

## The owner's emulator (emu8)

[emu8](https://github.com/hexadevti/emu8) has the same owner as this project.

| Here | From emu8 | Changes |
|---|---|---|
| `common/uifont.h` | `emu8/src/shared/fonts/uifont.h` | none |
| `esp32p4/main/board.h` | the pin map in `board.h` (`BOARD_PICOCALC_P4`) | only the pins this loader uses |
| `rp2xxx/src/lcd.c`, `esp32p4/main/lcd.c` | the ST7365P init sequence in `emu8/src/picocalc/display_picocalc.cpp` | the table only; the drivers are new |

`uifont.h` is generated from FabGL's `font_6x10.h` and `font_7x13.h`. The glyphs are the X11
"misc-fixed" 6x10 and 7x13 bitmap fonts, which are in the public domain.

## FatFs (RP2040 / RP2350 build) — FatFs licence (BSD-style, one clause)

By ChaN, <http://elm-chan.org/fsw/ff/>, release R0.16. The licence is in
[`rp2xxx/lib/fatfs/LICENSE.txt`](rp2xxx/lib/fatfs/LICENSE.txt) and in each file's header.

| Here | Changes |
|---|---|
| `rp2xxx/lib/fatfs/ff.c`, `ff.h`, `diskio.h`, `ffunicode.c`, `LICENSE.txt`, `00readme.txt` | none |
| `rp2xxx/lib/fatfs/ffconf.h` | settings only (see below) |

`ffconf.h` was started from the copy in [uf2loader](https://github.com/pelrun/uf2loader)
(`ui/lib/fatfs/ffconf.h`, GPL-3.0), which already set `FF_CODE_PAGE 437`, `FF_FS_NORTC 1`,
`FF_STRF_ENCODE 3`, `FF_PRINT_FLOAT 1` and `FF_VOLUME_STRS "SD","USB"`. This project changed it
to read-write, long file names in UTF-8 (`FF_USE_LFN 1`, `FF_LFN_UNICODE 2`), `FF_USE_CHMOD 1`,
`FF_USE_STRFUNC 1`, and turned off `FF_FS_RPATH` and `FF_PRINT_LLI`. The file itself, comments
included, is ChaN's.

The disk glue FatFs calls (`disk_read()`, `disk_write()` and the rest) is this project's own, in
`rp2xxx/src/sdcard.c`.

## Raspberry Pi Pico SDK — BSD-3-Clause

| Here | From the Pico SDK | Changes |
|---|---|---|
| `rp2xxx/pico_sdk_import.cmake` | `external/pico_sdk_import.cmake` | none |
| `rp2xxx/boot/boot2_pml.S` | `src/rp2040/boot_stage2/boot2_w25q080.S` | the SDK file is `#include`d at build time, not copied; this file only moves the jump target to the loader |

## ESP-IDF — Apache-2.0

By Espressif Systems, <https://github.com/espressif/esp-idf>, v5.5.

| Here | From ESP-IDF | Changes |
|---|---|---|
| `esp32p4/bootloader_components/main/bootloader_start.c` | `examples/custom_bootloader/bootloader_override/bootloader_components/main/bootloader_start.c` | the welcome message replaced by the check for the loader request: LP_STORE15 magic, BOOT button, PicoCalc keyboard over bit-banged I2C |
| `esp32p4/bootloader_components/main/CMakeLists.txt` | the same example's `CMakeLists.txt` | more `REQUIRES` |
| `esp32p4/bootloader_components/main/Kconfig` | the same example's `Kconfig` | the layout only; the options are new |

## TinyUSB — MIT

By Ha Thach (tinyusb.org), as shipped in the Pico SDK (`lib/tinyusb`).

| Here | Follows | Notes |
|---|---|---|
| `rp2xxx/src/usb_descriptors.c` | `examples/device/cdc_msc/src/usb_descriptors.c` | the same descriptor layout and TinyUSB's test VID `0xCAFE`; mass storage only, serial from the chip's unique ID |
| `rp2xxx/src/tusb_config.h` | the same example's `tusb_config.h` | the usual settings, mass storage only |

The mass-storage callbacks in `rp2xxx/src/usb_drive.c` and `esp32p4/main/usb_drive.c` are this
project's own. They share only TinyUSB's callback signatures and the standard SCSI sense codes.

## uf2loader — design and approach, no code (GPL-3.0)

[uf2loader](https://github.com/pelrun/uf2loader) by pelrun is the SD-card UF2 loader most
PicoCalc owners use, and the model for this project. PicoMultiLoader is written from scratch, but
it takes these ideas from uf2loader:

- **How the loader is entered.** At power-on the installed app starts straight away, and holding
  Up, F1 or F5 opens the menu instead. Both builds, including the ESP32-P4 bootloader, use the same
  keys.
- **How apps ask for the menu.** An app writes magic `0xe98cc638` to watchdog scratch 0 and a mode
  to scratch 1, then reboots. PicoMultiLoader honours this, so apps made for uf2loader return to
  its menu unchanged; `esp32p4/main/loader_api.h` is the same idea for the ESP32-P4.
- **Small flash part, menu on the SD card (RP2350).** A small boot stage in flash decides between
  app and menu, and the menu itself is a UF2 in the root of the SD card (uf2loader's
  `BOOT2350.UF2`, here `pml2350.uf2`), so the app gets almost all of the flash.
- **Flash layout (RP2350).** A partition table puts the app from 0x2000 to the end of flash, as
  uf2loader's `stage3/partitions.json.in` does (`rp2xxx/stub/pt.json`).
- **A modified boot2 (RP2040).** uf2loader's `stage3/boot2_custom.S` changes the SDK's W25Q080
  boot2 to reach the bootloader first; `rp2xxx/boot/boot2_pml.S` does the same by including the
  SDK file with a different jump target.
- **The menu's look.** A text file browser on the 320x320 screen: a title bar, the current path,
  folders shown as `[name]`, and `.uf2` files to pick and flash. App names come from the
  Pico SDK's binary_info block.
- **USB drive mode.** The SD card can be shared with a PC as a USB mass-storage drive from the
  menu.

Apart from `ffconf.h` (see FatFs above), a line-by-line comparison with uf2loader finds only the
PicoCalc pin numbers, Pico SDK calls and TinyUSB callback signatures in common.

## Interfaces followed, no code taken

These projects define behaviour this one is compatible with. Nothing from them is copied.

- **ClockworkPi PicoCalc**: the keyboard controller's I2C address (0x1F), registers and key
  codes, and the mainboard pin map, as documented by ClockworkPi.
- **Pico W access-point example** (`pico-examples/pico_w/wifi/access_point`, MIT, Damien
  P. George): `rp2xxx/src/dhcpserver.c` is a separate, smaller DHCP server and shares no code
  with it.
