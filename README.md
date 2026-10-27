**PicoMultiLoader** is an SD-card firmware loader for the **ClockworkPi PicoCalc**. Copy apps to
the PicoCalc's microSD card, browse to one on the LCD with the keyboard, and the loader flashes and
boots it. Hold **Up** (or F1/F5) while resetting to come back. It runs on three boards:

| Board in the Pico socket | Apps | Wi-Fi web file manager | Source | Build output |
|---|---|---|---|---|
| Waveshare ESP32-P4-WIFI6 | `.bin` | yes (through the ESP32-C6) | [esp32p4/](esp32p4/) | flashed over USB |
| Raspberry Pi Pico / Pico W (RP2040) | `.uf2` | no | [rp2xxx/](rp2xxx/) | `PicoMultiLoader-rp2040.uf2` (or `-classic`), flashed through BOOTSEL |
| Raspberry Pi Pico 2 W (RP2350) | `.uf2` | yes (CYW43439) | [rp2xxx/](rp2xxx/) | `PicoMultiLoader-rp2350.uf2` (boot stub) flashed through BOOTSEL + `pml2350.uf2` and/or `pml2350-classic.uf2` in the SD root |

All three run the same menus, built from [common/pml/](common/pml/): the graphical one (Home,
Files, control center, terminal, viewers; see [The menu](#the-menu)) and the
[classic](#the-classic-menu) file list. Each board folder only adds its drivers (`plat.h`, LCD and
keyboard buses, SD card, flashing, USB drive, Wi-Fi). The browser page is shared too
([common/web/index.html](common/web/index.html)). The first half of this README covers the
ESP32-P4 board; the RP2040 / RP2350 one is in
[Raspberry Pi Pico / Pico 2 W build](#raspberry-pi-pico--pico-2-w-build-rp2040--rp2350).

## Screenshots

The graphical menu looks the same on all three boards (these are from a Pico 2 W build). More
screens, and what every key and option does, are in [The menu](#the-menu).

| <img src="docs/screenshots/gui/home.png" width="260" alt="Home, Favourites tab"> | <img src="docs/screenshots/gui/home-all.png" width="260" alt="Home, All tab"> | <img src="docs/screenshots/gui/details.png" width="260" alt="An app's details before flashing"> |
|:---:|:---:|:---:|
| Home: favourites | Home: all the apps, with their icons | An app's details before flashing |
| <img src="docs/screenshots/gui/files.png" width="260" alt="Files with the preview pane"> | <img src="docs/screenshots/gui/files-preview.png" width="260" alt="Files previewing a text file"> | <img src="docs/screenshots/gui/files-marked.png" width="260" alt="Files with two marked items copied"> |
| Files (F3) with the preview pane | Previewing a text file | Marked items, copied with Ctrl+C |
| <img src="docs/screenshots/gui/control-center.png" width="260" alt="Control center"> | <img src="docs/screenshots/gui/setup.png" width="260" alt="Setup"> | <img src="docs/screenshots/gui/web.png" width="260" alt="Web file manager screen"> |
| Control center (F1) | Setup: Wi-Fi, start folder, time zone | Web file manager (F4) |
| <img src="docs/screenshots/gui/terminal.png" width="260" alt="Terminal"> | <img src="docs/screenshots/gui/text-reader.png" width="260" alt="Text reader"> | <img src="docs/screenshots/gui/idle.png" width="260" alt="Idle clock"> |
| Terminal (F5) | Text reader | Idle screen |

<p align="center">
  <img src="docs/screenshots/web-browser.png" width="820" alt="Web file manager in a browser"><br>
  <em>The web file manager (F4) in a browser</em>
</p>

## The menu

All three boards have this menu ([common/pml/ui.c](common/pml/ui.c)). Apps are `.uf2` files on
the Picos and `.bin` files on the ESP32-P4. The classic file list of earlier versions is still
available: see [The classic menu](#the-classic-menu).

### Keys

| Key | Does |
|---|---|
| Arrows, PgUp / PgDn, Home / End | Move |
| Enter | Run the app, open the folder or file |
| Esc / Backspace | Back |
| Tab | Next tab on Home; the action menu in Files |
| F1 | Control center: Wi-Fi, USB drive mode, backlight, idle screen, theme, menu style, and Setup |
| F2 | Add or remove a favourite |
| F3 | Files: the SD card, with copy / move / delete. Press again to go back to Home |
| F4 | Wi-Fi and the web file manager (Pico 2 W, ESP32-P4) |
| F5 | Terminal |
| F10 (Shift+F5) | Screenshot to `/pml/shots/shot-NNN.bmp` |

The serial console (115200 baud, see [Usage](#usage)) accepts the same keys, so the menu also
works from a PC without the PicoCalc keyboard.

### Start-up

<img src="docs/screenshots/gui/boot.png" width="260" align="right" alt="Start-up screen">

At switch-on the menu shows its logo and checks the SD card, the keyboard, the battery, Wi-Fi
(with autostart on) and how many apps are on the card. A missing part is shown in red; the menu
still opens. With no card, insert one and press Enter.

<br clear="right">

### Home

Home is the start screen. It shows the apps on the card as a grid of icons, four to a row, with
the selected app's version and description at the bottom.

* **What is an app:** every top-level folder with an `app.ini` or an app file in it, and every app
  file in the root. An `app.ini` gives the name, icon, category, version, author, description,
  which file to run and which boards it is for. The format and the icon rules are in
  [docs/apps.md](docs/apps.md). Without one, the folder name and a generic icon are shown.
* **Tabs** (Tab): **Favourites**, **Recent** (the apps run lately) and **All**. Home opens on the
  app run last.
* **Favourites:** F2 adds or removes the selected app; favourites have a yellow star. Both lists
  are plain text files, `/pml/favourites.txt` and `/pml/recents.txt`.
* **Search:** type to filter all the apps by name. Backspace deletes a letter, Esc clears.
* **Run:** Enter. The app already in flash has a green dot and starts at once; any other app is
  checked and flashed first (see [App details](#app-details)).
* **Folders of apps:** a folder with several apps and no `app.ini` saying which one to run opens
  on Home, with a **Back** tile. Back, Esc or Backspace go to the folder above; Tab goes straight
  back to the tabs. F3 opens that folder in Files.

| <img src="docs/screenshots/gui/home-search.png" width="260" alt="Searching the apps"> | <img src="docs/screenshots/gui/home-folder.png" width="260" alt="A folder of apps on Home"> |
|:---:|:---:|
| Typing searches all the apps | A folder with several apps, and the Back tile |

### App details

<img src="docs/screenshots/gui/details.png" width="260" align="right" alt="App details">

Before an app is flashed, the menu reads the file and shows what it is: the program name, version
and build date (from the UF2's `binary_info`, or the ESP-IDF app descriptor), its size, the chip
it was built for and how much of the app area it takes. **Enter** flashes and runs it, with a
progress bar; **Esc** goes back. For the app that is already installed, Enter just runs it and
**R** flashes it again. A file that can't run here (wrong chip or revision, too big, RAM-only
build) is not flashed: the screen says why.

<br clear="right">

### Files (F3)

The SD card as a file list, with a **preview pane** on the right: what is in a folder, the start
of a text file, a picture's size and format, or an app's details. The bar at the bottom shows how
full the card is.

* Enter or Right opens a folder or a file; Backspace or Left goes up, and at the top, back to
  Home. Esc or F3 also go back to Home. The last folder and file are remembered.
* Apps flash (through [App details](#app-details)); `.txt`, `.md`, `.log`, `.ini`, `.csv`, `.py`,
  `.bas` and `.pal` open in the text reader; `.bmp` in the image viewer.
* Only folders, apps and files the viewers open are listed, unless **Show all files** is on (Tab
  menu). Files that are only listed with it are dimmed.
* **Copy, move, delete:** Space marks items. **Ctrl+C** copies and **Ctrl+X** cuts the marked
  items (or the selected one), **Ctrl+V** pastes them into the folder shown, **Del** deletes after
  asking. Folders are copied and deleted with everything in them. Pasting a copy where the name is
  taken adds a number, e.g. `app (2).uf2`. Esc unmarks, then forgets what was copied.
* **Tab** opens the same actions as a menu: Copy, Cut (to move), Paste, Delete, Show all files.
* On the RP2350, the two menu files in the root (`pml2350.uf2`, `pml2350-classic.uf2`) can't be
  deleted or moved from here.

| <img src="docs/screenshots/gui/files-menu.png" width="260" alt="The Tab menu in Files"> | <img src="docs/screenshots/gui/files-delete.png" width="260" alt="Deleting a folder"> |
|:---:|:---:|
| The Tab menu | Deleting asks first |

### Text reader and image viewer

* **Text reader:** pages of text, Up/Down or PgUp/PgDn turn them, Home/End jump to the first and
  last. Markdown headings are shown in colour.
* **Image viewer:** a `.bmp` (uncompressed, 1, 4, 8, 16, 24 or 32 bits) on the whole screen,
  scaled to fit. Pictures with up to 16 colours are shown as they are; others are dithered to the
  16-colour screen. **G** toggles grey, Left/Right show the previous or next picture in the folder.
* Both close with Esc. In the terminal, `view <file>` opens them too.

| <img src="docs/screenshots/gui/text-reader.png" width="260" alt="Text reader"> | <img src="docs/screenshots/gui/image-viewer.png" width="260" alt="Image viewer"> |
|:---:|:---:|
| Text reader | Image viewer (a 24-bit BMP, dithered) |

### Control center (F1)

A panel over Home or Files. Up/Down pick a row, Left/Right change it, Enter opens or toggles it,
Esc closes. The battery charge is shown at the bottom, and the web address while Wi-Fi is on.

| Row | Options |
|---|---|
| Wi-Fi | On / Off: the web file manager (Pico 2 W, ESP32-P4), see [Web file manager](#web-file-manager-f4) |
| USB drive | Share the SD card with a PC as a USB drive, see [USB drive mode](#usb-drive-mode) |
| Backlight | 10–100 %, in 10 % steps (default 80 %) |
| Idle screen | After 1, 2, 5, 10 or 30 minutes without a key, or never (default 5 minutes) |
| Theme | Graphite, Phosphor, Amber, Paper, Clockwork, and your own (see [Themes](#themes)) |
| Menu style | Graphical / Classic: restarts into the [classic menu](#the-classic-menu) (ESP32-P4, RP2350) |
| Setup | Wi-Fi network, Wi-Fi autostart, start folder, time zone, and the on-screen help |

Every setting is saved in `picomultiloader.cfg` in the root of the SD card.

| <img src="docs/screenshots/gui/control-center.png" width="260" alt="Control center"> | <img src="docs/screenshots/gui/usb-drive.png" width="260" alt="USB drive mode"> |
|:---:|:---:|
| Control center, Wi-Fi on | USB drive mode |

### Setup and help

**Setup** (control center > Setup) has these rows; the Wi-Fi ones are explained under
[Setup](#setup):

| Row | Does |
|---|---|
| Network, Password | The Wi-Fi network the web server joins (type them in) |
| Show password | Show the password instead of `*****` |
| Scan for networks... | Pick a network from a list, strongest first |
| Save / Forget this network | Store the network, or remove it |
| Wi-Fi autostart | ON: start the web file manager in the background at switch-on |
| Start folder | Where Files opens: *last used*, or a folder you pick |
| Time zone | *auto* (from the web), or UTC−12:00 to UTC+14:00 |
| Help: how to use the loader | The on-screen guide |

The **help** is a scrollable guide to the whole menu: Home, app folders, Files, the viewers,
copying and deleting, installing the loader, getting back to it from an app, Wi-Fi, USB drive mode
and the serial console. Up/Down scroll, PgUp/PgDn page, Esc goes back.

| <img src="docs/screenshots/gui/setup.png" width="260" alt="Setup"> | <img src="docs/screenshots/gui/help.png" width="260" alt="On-screen help"> |
|:---:|:---:|
| Setup | On-screen help |

### Terminal (F5)

A command line on the SD card. Tab completes names, Up/Down recall earlier commands, Esc clears the
line, and F5 or `exit` closes it. `help <command>` explains one command.

| Command | Does |
|---|---|
| `ls`, `cd`, `pwd` | List a folder, change folder (no folder: the root), show where you are |
| `cat`, `view` | Show a text file; read it in pages, or show a `.bmp` |
| `apps`, `recent`, `fav` | The apps on Home, the ones run lately, the favourites |
| `run <app or file>` | Run an app, or flash and run a file |
| `files`, `home`, `ctl` | Open Files in this folder, go to Home, open the control center |
| `theme [name \| next]` | Show or change the theme |
| `date`, `tz [auto \| +hh:mm]` | The date and time; show or set the time zone |
| `wifi [on \| off]`, `ip` | Wi-Fi and the web server, its address (Pico 2 W, ESP32-P4) |
| `usb` | USB drive mode |
| `battery`, `free`, `df` | Battery charge, free memory, free space on the card |
| `about`, `uname` | The loader, the chip and the installed app; the system's name |
| `shot` | Save a screenshot in `/pml/shots` |
| `echo`, `history`, `clear` | Print text, the last commands, clear the screen |
| `reboot` | Restart: the installed app starts |

| <img src="docs/screenshots/gui/terminal.png" width="260" alt="Terminal"> | <img src="docs/screenshots/gui/terminal-help.png" width="260" alt="Terminal help"> |
|:---:|:---:|
| `about`, `df`, `free`, `date` | `help` |

### Web file manager

<img src="docs/screenshots/gui/web.png" width="260" align="right" alt="Web file manager screen">

On the Pico 2 W and the ESP32-P4, **F4** (or control center > Wi-Fi) starts Wi-Fi and a file
manager in the browser: upload, download, rename, delete, new folders, and flash an app from the
browser. The screen shows the address to open. Esc leaves it running in the background, with
green Wi-Fi bars in the status bar; F4 on this screen stops it. Details are in
[Web file manager (F4)](#web-file-manager-f4).

<br clear="right">

### Idle screen and clock

<img src="docs/screenshots/gui/idle.png" width="260" align="right" alt="Idle screen">

* **Idle screen:** after the time set in the control center without a key press, a dim clock on
  black with the backlight turned down, redrawn once a minute. 10 minutes later the backlight goes
  off. Any key wakes it (and does nothing else). Between keyboard polls the loader sleeps the CPU
  (WFE on the Picos, the FreeRTOS idle task on the ESP32-P4), so it draws little power when
  nothing is happening.
* **Date and time** (Pico 2 W, ESP32-P4): once Wi-Fi has joined your network, the loader sends one
  HTTP request to `ip-api.com`. The reply's `Date` header gives the time, and the body gives your
  connection's time zone, with summer time included. It syncs again every 6 hours, and the time
  shows in the middle of the status bar. You can set the time zone by hand under
  **Setup > Time zone**, or with `tz +01:00` in the terminal (`tz auto` goes back to automatic).
  Files the loader writes get real dates once the time is known.

<br clear="right">

### Themes

The control center's **Theme** row (or `theme next` in the terminal) cycles through five built-in
colour themes:

| <img src="docs/screenshots/gui/theme-graphite.png" width="180" alt="Graphite theme"> | <img src="docs/screenshots/gui/theme-phosphor.png" width="180" alt="Phosphor theme"> | <img src="docs/screenshots/gui/theme-amber.png" width="180" alt="Amber theme"> | <img src="docs/screenshots/gui/theme-paper.png" width="180" alt="Paper theme"> | <img src="docs/screenshots/gui/theme-clockwork.png" width="180" alt="Clockwork theme"> |
|:---:|:---:|:---:|:---:|:---:|
| Graphite (default) | Phosphor | Amber | Paper | Clockwork |

To add your own, put a `<name>.pal` file in `/pml/themes/` on the card. It holds 13 colours, as
`#rrggbb` or RGB565 `0x1234`, in this order: background, panel, field, line, dim, text, bright,
accent, dark accent, OK, warning, error, second accent. `;` starts a comment. The theme then shows
up in the cycle under its file name. App icons use the same roles (see [docs/apps.md](docs/apps.md)),
so they follow the theme.

### Screenshots and messages from apps

* **F10** (Shift+F5) or `shot` in the terminal saves the screen as `/pml/shots/shot-NNN.bmp`.
* **Messages from apps:** an app can leave a line in `/pml/exit.txt`, which the menu shows once
  when it opens. Alternatively, on the Picos it can set watchdog scratch 2 = `0x504D4C58` and
  scratch 3 = an exit code before rebooting into the menu; on the ESP32-P4 it calls
  `picomultiloader_exit_code()` from `loader_api.h`.

## The classic menu

Each build also makes a **classic** menu: the plain file list of earlier versions, without Home,
Files, the terminal or the viewers (about 45 KB more free RAM on the RP2350). It has the same flashing,
USB drive mode, Wi-Fi and web file manager.

Switching between the two:

* **ESP32-P4:** both menus are in the one loader. `ui=classic` in `picomultiloader.cfg` picks the
  classic one at switch-on. **Control center (F1) > Menu style** (graphical) and **Setup > Menu
  style** (classic) save the other one and restart into it.
* **RP2350:** both menus are files on the SD card, `pml2350.uf2` and `pml2350-classic.uf2`. The boot
  stub loads the classic one if `picomultiloader.cfg` has the line `ui=classic`, otherwise the
  graphical one; if the chosen file is missing it loads the other. Switch in the menu itself:
  **Control center (F1) > Menu style** in the graphical menu, **Setup > Menu style** in the classic
  one. Each restarts into the other, and refuses if the other file isn't on the card.
* **RP2040:** the loader is in flash, so flash the one you want through BOOTSEL:
  `PicoMultiLoader-rp2040.uf2` or `PicoMultiLoader-rp2040-classic.uf2`.

Its keys:

| Key | Does |
|---|---|
| Up / Down, PgUp / PgDn, Home / End | Move the selection |
| Enter / Right | Open a folder, or show the file's details; Enter there flashes and runs it |
| Backspace / Left | Parent folder / back |
| F1 | Setup: Wi-Fi network, start folder, menu style, and the on-screen help |
| F4 | Web file manager over Wi-Fi |
| F5 | USB drive mode |

| <img src="docs/screenshots/file-list.png" width="400" alt="File list on the SD card"> | <img src="docs/screenshots/details.png" width="400" alt="Details of a .bin before flashing"> |
|:---:|:---:|
| File list (the Wi-Fi icon shows the web server is on) | Checking an app before flashing it |
| <img src="docs/screenshots/web-server.png" width="400" alt="Web file manager screen"> | <img src="docs/screenshots/setup.png" width="400" alt="Setup screen"> |
| Web file manager (F4) | Setup (F1): Wi-Fi network and start folder |
| <img src="docs/screenshots/help.png" width="400" alt="On-screen help"> | <img src="docs/screenshots/usb-drive.png" width="400" alt="USB drive mode"> |
| On-screen help | USB drive mode (F5) |

## How it works

```
0x002000  bootloader   custom 2nd-stage bootloader (esp32p4/bootloader_components/main)
0x008000  partition table
0x009000  nvs
0x00F000  otadata      which app slot to boot
0x020000  factory  2M  this loader
0x220000  ota_0   10M  the app you picked from the SD card
0xC20000  storage ~20M FAT data partition, free for apps to use      (32 MB flash)
```

* **Loader** (`esp32p4/main/` + `common/pml/`): the menu on the PicoCalc LCD lists the apps,
  folders and `*.bin` files on the SD card, and checks the selected image (ESP32-P4 chip ID, chip revision, app
  descriptor). It then writes the image to `ota_0` with the ESP-IDF OTA API, which verifies
  the checksum and SHA256, marks it bootable and reboots.
* **Bootloader** (`esp32p4/bootloader_components/main`): the stock ESP-IDF bootloader plus a check
  for 500 ms after reset. If a PicoCalc key (Up, F1 or F5) or the board's BOOT button is
  pressed in that window, or an app asked for the loader in software, it boots the `factory`
  loader instead of the app. The keyboard is read over bit-banged I2C.
* Loaded apps are **not modified**. The flash MMU maps whichever app partition is booted,
  so any ESP-IDF, Arduino or MicroPython build for the ESP32-P4 runs from `ota_0`.

### Supported files

* A plain app image, e.g. `build/<project>.bin` from ESP-IDF or the Arduino
  "Export compiled binary" output.
* A merged/full-flash image from `esptool merge_bin`, such as `*.factory.bin`. The loader
  reads the partition table inside the file and extracts only the app, so your bootloader
  is never overwritten.

## Hardware

### The board: Waveshare ESP32-P4-WIFI6

<p align="center">
  <img src="https://www.waveshare.com/w/upload/2/28/360px-ESP32-P4-WIFI6-1.jpg" width="300" alt="Waveshare ESP32-P4-WIFI6">
</p>

The [ESP32-P4-WIFI6](https://www.waveshare.com/wiki/ESP32-P4-WIFI6) has the same footprint and
pin layout as a Raspberry Pi Pico, so it goes in the PicoCalc's Pico socket in place of a Pico /
Pico 2. It carries an ESP32-P4NRW32 (32 MB PSRAM in the package), 32 MB flash, an ESP32-C6
co-processor for Wi-Fi 6 / Bluetooth LE, MIPI display and camera connectors, a microSD slot and a
4-pin USB 2.0 high-speed header.

| Onboard parts | Pinout |
|:---:|:---:|
| <img src="https://www.waveshare.com/w/upload/d/d5/ESP32-P4-WIFI6-details-intro.jpg" width="420" alt="ESP32-P4-WIFI6 onboard parts"> | <img src="https://www.waveshare.com/w/upload/e/e2/ESP32-P4-WIFI6-details-inter.jpg" width="420" alt="ESP32-P4-WIFI6 pinout"> |

*Pictures: Waveshare. They are linked from the [Waveshare wiki](https://www.waveshare.com/wiki/ESP32-P4-WIFI6), not copied into this repo.*

### Fitting it in the PicoCalc (case modification)

The PicoCalc's case has a small opening for the Pico's micro-USB port. The ESP32-P4-WIFI6 has a
larger **USB-C** connector in that spot, so the board doesn't sit in the socket, and no cable fits,
until the opening is made bigger:

1. Open the PicoCalc and take out the Pico.
2. Enlarge the opening for the Pico's USB port (the right-hand one in the photo; the left one is
   the PicoCalc's own USB-C) to fit the board's USB-C connector and a cable plug. A craft knife,
   small files or a rotary tool work. Take a little off at a time and test-fit the board.
3. Clean up the plastic dust, fit the ESP32-P4-WIFI6 in the Pico socket with the USB-C port
   facing the opening, and close the case.

This USB-C port is the board's serial/flashing port (CH343). Use it to flash the loader and to
reach the serial console.

<p align="center">
  <img src="docs/picocalc-case-mod.jpg" width="320" alt="PicoCalc case with the Pico USB opening enlarged for the board's USB-C port"><br>
  <em>The enlarged opening (right) next to the PicoCalc's own USB-C port (left).</em>
</p>

### ESP32-P4 vs RP2040 vs RP2350

How the P4 board compares with the chips on the Picos that the PicoCalc normally takes:

| | **ESP32-P4** (Waveshare ESP32-P4-WIFI6) | **RP2040** (Pico / Pico W) | **RP2350** (Pico 2 / Pico 2 W) |
|---|---|---|---|
| CPU | 2× RISC-V @ 360 MHz (chip v1.x) / 400 MHz (v3.x), plus a RISC-V low-power core @ 40 MHz | 2× Arm Cortex-M0+ @ 133 MHz (200 MHz certified) | 2× Arm Cortex-M33 or 2× Hazard3 RISC-V @ 150 MHz |
| Floating point | Single-precision FPU, SIMD/AI instructions | None (software) | Single-precision FPU (M33), double-precision coprocessor |
| On-chip SRAM | 768 KB + 32 KB low-power + 8 KB TCM | 264 KB | 520 KB |
| PSRAM | 32 MB in the package | None | None on the Pico 2 (supported via QSPI) |
| Flash on the board | 32 MB | 2 MB | 4 MB |
| Wireless | Wi-Fi 6 (2.4 GHz) + Bluetooth 5 LE, through the ESP32-C6 | Pico W: Wi-Fi 4 (2.4 GHz) + Bluetooth 5.2 (CYW43439) | Pico 2 W: Wi-Fi 4 (2.4 GHz) + Bluetooth 5.2 (CYW43439) |
| USB | USB 2.0 **high-speed** OTG (480 Mbit/s) + full-speed; USB-C serial via CH343 | USB 1.1 full-speed (12 Mbit/s) device/host | USB 1.1 full-speed (12 Mbit/s) device/host |
| Programmable I/O | No PIO (has a parallel I/O peripheral) | 2 PIO blocks, 8 state machines | 3 PIO blocks, 12 state machines, HSTX |
| Display / camera / media | MIPI-DSI, MIPI-CSI, ISP, H.264 encoder, JPEG codec, 2D pixel accelerator | — | — (HSTX can drive DVI) |
| SD card | SD 3.0 host controller (SDIO slot on the board) | SPI or PIO | SPI or PIO |
| GPIO on the chip | 55 | 30 | 30 (QFN-60) / 48 (QFN-80) |
| Security | Secure boot, flash encryption, crypto accelerators | — | Secure boot, TrustZone, OTP, glitch detectors |
| PicoCalc firmware loader | **PicoMultiLoader** (`.bin` from SD) | **PicoMultiLoader** (`.uf2` from SD) | **PicoMultiLoader** (`.uf2` from SD) |

### Pins used by the loader

The pin map is in `esp32p4/main/board.h` (taken from emu8's `BOARD_PICOCALC_P4`):

| Part | Bus | Pins |
|---|---|---|
| LCD ST7365P 320x320 | SPI2, 40 MHz | SCK 3, MOSI 2, CS 7, DC 24, RST 25 |
| Keyboard / battery / backlight (STM32 @ 0x1F) | I2C 10 kHz | SDA 50, SCL 49 |
| microSD | SPI3, 20 MHz | SCK 46, MISO 48, MOSI 33, CS 47 (powered by on-chip LDO 4) |

GPIO24/25 are the USB-Serial-JTAG pads, so the console is UART only (GPIO37/38, the CH343
USB port). The keyboard and LCD are powered by the PicoCalc, not the P4. If the P4 runs from
USB with the PicoCalc switched off, the loader keeps polling, and it re-initialises the panel
once the keyboard answers.

## Build and flash the loader (once, over USB)

```powershell
. $HOME\esp\esp-idf\export.ps1
cd esp32p4
idf.py set-target esp32p4
idf.py build
idf.py -p COMx flash monitor
```

**Chip revision:** `esp32p4/sdkconfig.defaults` targets v3.x silicon. Run `esptool.py chip_id` to
check your chip. On a v1.x board, uncomment `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`, delete
`sdkconfig`, and rebuild. The app images you load must be built for the same revision
family, and the loader rejects ones that aren't.

## Usage

1. Format a microSD card as FAT32 and copy `.bin` files to it, in any folder.
2. Switch the PicoCalc on. The loader starts when nothing is installed yet. Otherwise,
   **hold Up (or F1/F5) and press reset / power-cycle**.
3. The keys and screens are described in [The menu](#the-menu) (and
   [The classic menu](#the-classic-menu)). The serial console on the USB-C port (115200 baud)
   accepts the same keys (arrows, Enter, Backspace, Esc, F1-F5 and typed text), so it works
   without the PicoCalc keyboard too. Ctrl+P prints the screen as text; `tools/screenshots/`
   turns that into pictures like the [classic menu](#the-classic-menu) ones.

The board's BOOT button also works: press it within 0.5 s *after* releasing RESET. Holding
it *while* releasing RESET enters ROM download mode instead, because GPIO35 is a strapping pin.

### USB drive mode

Choose **USB drive** in the control center (F1; F5 in the classic menu) to share the SD card
with a PC. It needs the board's **4-pin USB
OTG 2.0 HS header** (VBUS, D-, D+, GND) wired to a USB cable going to the PC. The board's USB-C
port goes to the CH343 USB-to-serial chip and can't carry USB storage. The card then appears as
a USB drive. The screen shows *Waiting for the PC*, *Connected* or *Cable unplugged*.

* While the PC has the card, the loader doesn't touch it.
* **Eject the drive on the PC** when you're done. The loader takes the card back and refreshes
  the list by itself. Esc also leaves, but only press it after ejecting, or you may lose
  unsaved writes.
* It uses the P4's high-speed USB port (TinyUSB, `espressif/esp_tinyusb`). The MSC callbacks
  are in `esp32p4/main/usb_drive.c` and pass raw SD sectors through. esp_tinyusb's own storage layer is
  deliberately not used, because it formats the card if it fails to mount it.

### Setup

**Setup** (control center > Setup; F1 in the classic menu) chooses the Wi-Fi network the web
server joins:

* Type the **Network** name and **Password** (Backspace deletes; *Show password* toggles the
  mask), or pick **Scan for networks...** and choose one from the list.
* **Save** stores them in `picomultiloader.cfg` on the SD card, with the other settings.
  **Forget this network** removes them. (Earlier versions kept the network in NVS; it is moved to
  the card the first time it is read.)
* Up/Down or Tab move between rows; Home/End jump to the first/last row.
* **Wi-Fi autostart** (ON/OFF, Enter toggles) starts the web file manager in the background at
  switch-on, as if you pressed F4 and then Esc. It is OFF by default.
* **Start folder** picks the folder the file list opens in at switch-on: browse to it (Enter
  opens a folder, Left goes up) and choose *Use this folder*. *Remember the last folder*
  (the default) reopens wherever you were last.
* Only 2.4 GHz networks work, because the ESP32-C6 has no 5 GHz radio. Passwords must be 8–64
  characters, or empty for an open network.

The web page has the same setting under *Wi-Fi settings*. While the web server is on, scanning
is disabled, and a changed network is used the next time the server starts.

The last row, **Help: how to use the loader**, opens a scrollable on-screen guide. It covers the
keys, getting back to the loader, which files work, Wi-Fi, the web file manager, USB drive mode
and the serial console. Up/Down scroll, PgUp/PgDn page, Esc goes back.

### Web file manager (F4)

Press **F4** to manage the SD card from a browser over Wi-Fi:

1. If a network is set up (F1), the loader joins it and shows `http://<ip>/`. On most networks
   `http://picocalc.local/` works too.
2. If no network is set up, or it can't join within 20 s, it starts its own access point
   instead. The access point is called `PicoMultiLoader-XXXX`, its password is shown on screen,
   and you open `http://192.168.4.1/`. The password is random, made once, and kept, so a phone
   can remember the network. The screen says why joining failed (*not found* or *wrong
   password?*).
3. In the page you can:
   * browse folders, then **upload** files (button or drag and drop, several at once, with
     progress), **download**, **rename**, **delete** (folders recursively) and create folders;
   * **Flash** a `.bin`, which stops the server and flashes and boots it with the usual checks,
     with progress on the PicoCalc screen.
4. **Esc** goes back to the file list and leaves the server running in the background. A Wi-Fi
   icon in the top bar shows it is on: green when ready, blue while starting, amber while
   reconnecting, red if it failed. The file list refreshes by itself when the browser changes
   files. Press **F4** again to see the server screen, and **F4 there** to stop it. Flashing a
   file (from the PicoCalc or the browser) and USB drive mode stop it first.

How it works: the P4 has no radio, so Wi-Fi goes through the board's ESP32-C6 over SDIO
(`espressif/esp_wifi_remote` + `espressif/esp_hosted`). The pins are the ESP32-P4 Function EV
board defaults: CLK 18, CMD 19, D0–D3 14–17, C6 reset 54. The server is `esp_http_server`, and
the page is `common/web/index.html`, embedded in the firmware. There is **no login**, so anyone on
the same network can change the card while F4 is on. Only use it on networks you trust, and stop
it when you're done.

The C6 on these boards ships with old ESP-Hosted firmware (reported as 0.0.0). The 3.x host
works with it in compatibility mode, and logs `major version mismatch`, which is harmless.

### Returning to the loader from your own app

Copy `esp32p4/main/loader_api.h` into your project and call `picomultiloader_reboot_to_menu()`
(the old name `binloader_reboot_to_menu()` still works). It writes
a magic value to `LP_SYSTEM_REG_LP_STORE15_REG`, which survives a software reset, and then
restarts. This is the equivalent of the PicoCalc UF2 Loader's watchdog-scratch command.

To leave a message for the menu, call `picomultiloader_exit_code(code)` first: the menu shows the
code once when it opens (LP_SYSTEM store registers 13 and 14). Writing a line to `/pml/exit.txt`
on the card works too.

## Building apps to load

Any ESP32-P4 app image works. The rules are the same for ESP-IDF and the Arduino IDE:

* **Chip revision:** build for the same revision family as the loader (v3.x by default, see
  [Build and flash the loader](#build-and-flash-the-loader-once-over-usb)). The loader
  checks this and shows why it rejects an image on its details screen.
* **Partition table:** your app's own table, bootloader and partition settings are ignored.
  The app runs from `ota_0` (up to 10 MB) with the loader's [partitions.csv](esp32p4/partitions.csv):
  `nvs`, `phy_init` and a 20 MB FAT partition named `storage`.
* **Flash size:** 32 MB. **Rollback:** not needed; the app doesn't have to confirm itself.
* Copy the `.bin` to the SD card (F4 web upload or F5 USB drive mode work too), pick it, Enter.

### ESP-IDF

```sh
idf.py set-target esp32p4
idf.py menuconfig   # Serial flasher config > Flash size: 32 MB
                    # Component config > Hardware Settings > Chip revision: match the loader
idf.py build
```

Copy `build/<project>.bin` to the SD card. `build/<project>.factory.bin` or any
`esptool merge_bin` output works too. Copying [partitions.csv](esp32p4/partitions.csv) into your
project (with `CONFIG_PARTITION_TABLE_CUSTOM=y`) keeps `esp_partition_find()` results the
same as when the app runs under the loader.

### Arduino IDE

1. Install the *esp32* boards package by Espressif (3.x) and select an ESP32-P4 board
   (e.g. *ESP32P4 Dev Module*).
2. Tools menu: *Flash Size* 32MB, *PSRAM* enabled. If your core version has a chip revision
   option, pick the one that matches the loader; older cores only build for v1.x silicon,
   which a v3.x loader rejects. *Partition Scheme* doesn't matter (see above).
3. *Sketch > Export Compiled Binary*. This writes `build/<board>/<sketch>.ino.bin` (and
   `<sketch>.ino.merged.bin`) into the sketch folder. Copy either one to the SD card.

File systems: `Preferences` works as is (it uses `nvs`). SPIFFS and LittleFS look for a
partition named `spiffs`, which doesn't exist, so use the `storage` partition instead:

```cpp
#include <FFat.h>
FFat.begin(true, "/ffat", 10, "storage");          // FAT, the partition's native format

#include <LittleFS.h>
LittleFS.begin(true, "/littlefs", 10, "storage");  // formats it as LittleFS on first use
```

Wi-Fi in sketches goes through the board's ESP32-C6, like the loader's web server.
To go back to the loader from a sketch, put `loader_api.h` in the sketch folder (see
[Returning to the loader](#returning-to-the-loader-from-your-own-app)).

## Configuration (`idf.py menuconfig`, ESP32-P4)

* *PicoMultiLoader: bootloader*: trigger window, BOOT button GPIO and debounce, keyboard
  trigger on/off and its I2C pins.
* *PicoMultiLoader*: the folder the browser starts in the very first time, before a folder has
  been remembered or set in Setup.

## Limitations / notes (ESP32-P4)

* Apps use the loader's partition table. Apps that expect `nvs`, `phy_init` or a FAT
  partition named `storage` will find them. Apps that need other data partitions
  (e.g. `spiffs`) won't, so edit `esp32p4/partitions.csv` to suit. The table must stay at 0x8000.
* App rollback is disabled on purpose. Loaded apps don't know they need to confirm
  themselves, so with rollback on they would be reverted.
* Secure boot and flash encryption are not supported.
* The custom bootloader must fit in 0x2000–0x8000. With the keyboard trigger it only fits
  with `CONFIG_BOOTLOADER_LOG_LEVEL_WARN` (about 0x9E0 bytes free).

## Raspberry Pi Pico / Pico 2 W build (RP2040 / RP2350)

The [rp2xxx/](rp2xxx/) folder builds the same loader for a Raspberry Pi Pico (RP2040, no Wi-Fi)
or a Pico 2 W (RP2350, with the web file manager) in the PicoCalc. No other bootloader is needed.
On the RP2040 the whole loader is the firmware in flash. On the RP2350 only an 8 KB boot stub is
in flash (apps get `0x2000` to the end of flash), and the full loader is the file `pml2350.uf2` on
the SD card, which the stub loads into RAM when the menu is wanted.

* At power-on it watches the keyboard for half a second. If **Up, F1 or F5** is held, an app
  asked for the menu, or no app is installed, it shows the menu; otherwise it starts the app.
* It lists folders and `*.uf2` files, checks the selected file (UF2 family for the chip,
  addresses, name/version from `binary_info`), writes it into the app area, verifies every page,
  and reboots into it.
* Apps can ask for the menu: set watchdog scratch 0 = `0xe98cc638` and scratch 1 = `1`, then
  do a watchdog reboot.

Flash layout:

| Chip | Layout |
|---|---|
| RP2040 (2 MB) | `0x000000` boot2 (ours, enters the loader) + app, up to `0x1BF000` · `0x1BF000` app record · `0x1C0000` loader (256 KB) |
| RP2350 (4 MB) | `0x000000` boot stub + partition table (8 KB) · `0x002000` app (3832 KB), started with the bootrom's `chain_image`, which maps it to `0x10000000` · `0x3C0000` Wi-Fi files (256 KB: the CYW43439 firmware and the web page). The loader runs from RAM. |

Apps are normal Pico SDK flash builds linked at `0x10000000`; RAM-only (`no_flash`) UF2s are
refused.

### Install

1. Hold **BOOTSEL** on the Pico, plug it into USB, and copy `PicoMultiLoader-rp2040.uf2` (Pico)
   or `PicoMultiLoader-rp2350.uf2` (Pico 2 W) to the `RPI-RP2` / `RP2350` drive. (The RP2350
   file uses the UF2 "absolute" family, so it is written to the start of flash whatever partition
   table is there.)
2. **RP2350 only:** copy `pml2350.uf2` (and `pml2350-classic.uf2`, if you want to be able to switch
   to the [classic menu](#the-classic-menu)) to the root of the SD card. Without either one the menu
   can't open: the stub starts the app, or drops into BOOTSEL if there is none.
3. Put the Pico back in the PicoCalc and switch it on: with no app installed, the menu opens.
4. **Pico 2 W only:** the first time Wi-Fi is turned on (F4, or a network scan in Setup), the
   loader copies the Wi-Fi files from `pml2350.uf2` (or `pml2350-classic.uf2`, which carries the same
   ones) into the last 256 KB of flash. That takes a few
   seconds and happens again only when a new `pml2350.uf2` brings different files. The loader
   keeps the CYW43439 firmware there rather than in RAM, which leaves it a ~205 KB heap. If the
   installed app has written data into that area, the loader asks first, because installing erases it.

On the RP2350, apps can also be installed through BOOTSEL: a normal RP2350 UF2 copied to the
drive goes into the app partition at `0x2000`.

### Caveats

* **Apps that keep data in flash.** On the RP2040 the loader sits in the last 256 KB of the
  2 MB flash: an app that writes there (a file system at "the end of flash", for example)
  overwrites the loader, which then has to be flashed again through BOOTSEL. On the RP2350 the
  app runs from a window mapped to `0x10000000`, so apps with a file system at the end of flash
  (Arduino-Pico ones, for example) work. On the Pico 2 W, though, the last 256 KB holds the Wi-Fi
  files: apps get 3832 KB, and an app that writes there loses that data when the Wi-Fi files are
  installed (the loader asks first), and turns Wi-Fi off until they are installed again.
* **The clock** is set from the web and is lost when an app runs or the power goes off (there is
  no RTC). It comes back the next time Wi-Fi is on.
* A power cut while flashing leaves no valid app; the menu opens at the next start. On the
  RP2040, a cut while sector 0 is being rewritten can also lose boot2: flash the loader again
  through BOOTSEL. (On the RP2350 the app's first sector is written last, so a half-written app
  is never started.)

### Build

Needs the [Pico SDK](https://github.com/raspberrypi/pico-sdk) 2.1 or later, the Arm GNU
toolchain, CMake and Ninja.

```sh
cd rp2xxx
export PICO_SDK_PATH=/path/to/pico-sdk
cmake -B build-rp2040 -G Ninja -DPICO_BOARD=pico      # -> build-rp2040/PicoMultiLoader-rp2040.uf2 + PicoMultiLoader-rp2040-classic.uf2
cmake -B build-rp2350 -G Ninja -DPICO_BOARD=pico2_w   # -> build-rp2350/PicoMultiLoader-rp2350.uf2 + pml2350.uf2 + pml2350-classic.uf2
cmake --build build-rp2040
cmake --build build-rp2350
```

If the SDK builds its own `picotool` and that crashes (it happens with very new host compilers),
point the build at a working one with `-DPICOTOOL_EXE=/path/to/picotool`.

### Differences from the ESP32-P4 build

* **Settings** (Wi-Fi network, AP password, start folder, autostart, last folder, theme, idle
  screen, time zone) are kept in `/picomultiloader.cfg` on the SD card, as on the ESP32-P4
  (which keeps only its access point password in NVS).
* **USB drive mode** (control center, F1) uses the Pico's own micro-USB port (full speed, 12 Mbit/s). The serial
  console is on the PicoCalc's USB-C port (UART0, 115200).
* **Web file manager (F4)**, Pico 2 W only: lwIP and the CYW43439 driver from the Pico SDK, with
  the same page, API, `picocalc.local` name, `PicoMultiLoader-XXXX` access point and DHCP server
  at `192.168.4.1`. Up to 3 browser connections at a time.
* The browser page is stored gzipped to keep the loader small.

### Pins (PicoCalc mainboard)

The pin map is in `rp2xxx/src/board.h`:

| Part | Bus | Pins |
|---|---|---|
| LCD ST7365P 320x320 | SPI1 | SCK 10, MOSI 11, MISO 12, CS 13, DC 14, RST 15 |
| Keyboard / battery / backlight (STM32 @ 0x1F) | I2C1 10 kHz | SDA 6, SCL 7 |
| microSD | SPI0, 25 MHz | SCK 18, MOSI 19, MISO 16, CS 17, card detect 22 |
| Serial console | UART0 | TX 0, RX 1 |

### Credits and licensing

The Pico and Pico 2 W builds are inspired by [uf2loader](https://github.com/pelrun/uf2loader) by
pelrun, the SD-card UF2 loader many PicoCalc owners already use. Many thanks to pelrun for making
it and sharing it. PicoMultiLoader does not include uf2loader code; it follows the same app
interface so apps made for uf2loader work, and its FatFs `ffconf.h` started from uf2loader's.

The RP2040 boot2 is the Pico SDK's W25Q080 boot2 (BSD-3-Clause) with a different jump target.

[`THIRD-PARTY.md`](THIRD-PARTY.md) lists every file taken from elsewhere (FatFs, the Pico SDK,
ESP-IDF, TinyUSB) with its licence and what was changed.
