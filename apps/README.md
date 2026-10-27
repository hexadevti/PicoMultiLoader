# Bundled apps

The PicoMultiLoader's own tools for the PicoCalc with a Raspberry Pi Pico (RP2040) or Pico 2 / 2 W
(RP2350): Music Player, Text Editor, Image Editor and Calculator. They are built apart from the
loader, but they reuse its drivers and look. Each app is a normal flash build that the loader
starts like any other app (see [docs/apps.md](../docs/apps.md)).

## Build

You need the Pico SDK (2.1 or later, with `PICO_SDK_PATH` set), the Arm GNU toolchain, CMake and Ninja.

```sh
cmake -B build-rp2040 -G Ninja -DPICO_BOARD=pico
cmake -B build-rp2350 -G Ninja -DPICO_BOARD=pico2
cmake --build build-rp2040
cmake --build build-rp2350
```

To use a picotool you already have rather than letting the SDK build one, add
`-DPICOTOOL_EXE=<path to picotool>`.

Both builds put their files in `dist/`, one folder per app:

```
dist/TextEditor/app.ini
dist/TextEditor/icon.bmp
dist/TextEditor/text-editor-rp2040.uf2
dist/TextEditor/text-editor-rp2350.uf2
```

Copy the folders to the root of the SD card. `python ../tools/pml_app.py check dist/*` checks them.

## Layout

| Path | What it is |
| --- | --- |
| `sdk/pmlapp.h` | The shared library: start-up, keys, the title and key bars, dialogs, a file picker, settings in the app's folder, and exit to the menu |
| `sdk/lcd8.c` | The `lcd.h` drawing calls on an 8-bit framebuffer. Palette entries 0 to 15 are the user's theme; 16 to 255 are free for the app |
| `sdk/audio.h` | Stereo PWM sound on GPIO 26 and 27, fed by DMA from a queue of buffers |
| `CMakeLists.txt` | The build, and `pml_app()` |
| `make_icons.py` | Draws each app's `icon.bmp` from ASCII art |
| `<app>/` | One folder per app: its sources, `app.ini`, `icon.bmp` and a one-line `CMakeLists.txt` |

## Adding an app

1. Make a folder, for example `clock/`, with your sources.
2. Write an `app.ini` like [calculator/app.ini](calculator/app.ini). The build reads `name` and
   `version` from it. Name the binaries `<target>-rp2040.uf2` and `<target>-rp2350.uf2`.
3. Draw an icon: add it to `make_icons.py` and run `python make_icons.py`, or use
   `tools/pml_app.py icon`.
4. Add `clock/CMakeLists.txt`:
   ```cmake
   pml_app(clock Clock SOURCES clock.c LIBS m)
   ```
   The first argument is the target, and the second is the folder on the SD card. `LIBS` is
   optional; any Pico SDK library can go there.
5. Add `add_subdirectory(clock)` to the end of `CMakeLists.txt`.

A minimal app:

```c
#include "pmlapp.h"

int main(void)
{
    app_init();
    for (;;) {
        app_header(APP_NAME, NULL);
        app_clear_body();
        lcd_text(10, APP_TOP + 10, "Hello", C_FG, FONT_LARGE);
        app_footer("Esc Quit");
        lcd_flush();
        if (app_wait_key() == KEY_ESC) {
            app_exit(NULL);
        }
    }
}
```

`APP_NAME`, `APP_VERSION` and `APP_DIR` (the app's folder on the card, for example `"/Clock"`)
are defined by the build. Keep the app's files in `APP_DIR`; `app_setting_get()` and
`app_setting_set()` already do this.
