# App template and start kits

* [app.ini](app.ini) and [icon.bmp](icon.bmp): the metadata for any app, whatever it is built
  with. See [docs/apps.md](../../docs/apps.md).
* [rp2350/](rp2350/): a start kit in C for the **Pico 2 / Pico 2 W** (Pico SDK).
* [esp32p4/](esp32p4/): a start kit in C for the **Waveshare ESP32-P4-WIFI6** (ESP-IDF 5.5).

Each start kit is a complete, separate project: copy its folder to start your own app. Both build
the same demo, four pages switched with F1 to F4 (or Left/Right):

| Page | Shows |
|---|---|
| F1 Text | Both fonts, colours, scaled text with a shadow, a line you type in, uptime |
| F2 Shapes | Lines, rectangles, rounded rectangles, circles, triangles, a hue gradient |
| F3 Bounce | Animated balls with gravity and the frame rate (Up/Down: more or fewer balls) |
| F4 Chart | An animated line plot and bar chart (Up/Down: frequency) |

Esc returns to PicoMultiLoader's menu.

## What is in a kit

| File | What it is | Same in both kits |
|---|---|---|
| `main.c` | The demo. Replace it with your app | yes |
| `gfx.h`, `gfx.c` | Drawing on the frame buffer: pixels, lines, rectangles, circles, triangles, text, colour blending, clipping | yes |
| `font.h` | Two public-domain bitmap fonts, 6x10 and 7x13 | yes |
| `picocalc.h`, `picocalc.c` | The PicoCalc: `pc_init()`, `pc_flush()`, `pc_key()`, `pc_battery()`, `pc_backlight()`, `pc_millis()`, `pc_exit_to_menu()` | yes |
| `port_rp2350.c` / `port_esp32p4.c` | The board: screen over SPI with DMA, keyboard over I2C, time, return to the menu | no |
| `app.ini`, `icon.bmp` | What the loader's Home shows. The build reads `name` and `version` from `app.ini` | |

The app draws into a 320 x 320 RGB565 frame buffer (200 KB) and sends all of it with
`pc_flush()`, about 20 frames a second. A program for one kit runs on the other with no change
except the port.

```c
#include "gfx.h"
#include "picocalc.h"

int main(void) // ESP-IDF: void app_main(void)
{
    pc_init();
    gfx_init(pc_framebuffer(), PC_W, PC_H);
    for (;;) {
        gfx_clear(C_BLACK);
        gfx_text(10, 10, "Hello", C_CYAN, FONT_LARGE, 3);
        pc_flush();
        if (pc_key() == KEY_ESC) {
            pc_exit_to_menu();
        }
    }
}
```

## RP2350 (Pico 2 / Pico 2 W)

You need the Pico SDK 2.1 or later (`PICO_SDK_PATH` set), the Arm GNU toolchain, CMake and Ninja.

```sh
cd rp2350
cmake -B build -G Ninja            # add -DPICOTOOL_EXE=<path> to use a picotool you have
cmake --build build
```

The card folder is `dist/StartKit/`: `app.ini`, `icon.bmp` and `startkit.uf2`. A built copy is
in the repository, so you can try the demo without building it. `printf()` goes to the
PicoCalc's USB-C port at 115200 baud.

## ESP32-P4

You need ESP-IDF 5.5 (run its `export` script first).

```sh
cd esp32p4
idf.py build
```

The card folder is `dist/StartKitP4/`: `app.ini`, `icon.bmp` and `startkit-p4.bin`. A built copy
is in the repository, so you can try the demo without building it. Logs go to UART0 at 115200
baud. Notes:

* The PicoCalc screen uses GPIO 24 and 25, the USB-Serial-JTAG pads, so the console must stay off
  USB-Serial-JTAG (`CONFIG_ESP_CONSOLE_SECONDARY_NONE` in `sdkconfig.defaults`).
* v3.x silicon is the default. For an older v1.x chip, set `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`
  in `sdkconfig.defaults` and delete `sdkconfig` before building.
* The board's 32 MB PSRAM is not turned on. Enable `CONFIG_SPIRAM` if your app needs it.
* Started from PicoMultiLoader, the app runs from the loader's 10 MB `ota_0` partition.

## Making it yours

1. Copy the kit folder and rename it.
2. Edit `app.ini`: `name`, `author`, `version`, `desc`. If you rename the binary, change it in
   `CMakeLists.txt` too.
3. Replace `main.c` with your app. Draw an icon with `python tools/pml_app.py icon`.
4. Build, then check the card folder: `python tools/pml_app.py check dist/<folder>`.
