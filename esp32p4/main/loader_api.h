// loader_api.h - copy this header into your own ESP32-P4 app to return to PicoMultiLoader in software
// (the equivalent of the PicoCalc UF2 Loader's "watchdog scratch" command).
//
//     #include "loader_api.h"
//     picomultiloader_reboot_to_menu();
//
// It leaves a magic value in an LP_SYSTEM store register, which survives a software reset, and
// restarts. PicoMultiLoader's custom bootloader sees it and boots the loader instead of the app.
// Without PicoMultiLoader installed, this is a plain restart.
#pragma once

#include <stdint.h>
#include "esp_system.h"
#include "soc/soc.h"
#include "soc/lp_system_reg.h"

#define PICOMULTILOADER_MAGIC_REG LP_SYSTEM_REG_LP_STORE15_REG
#define PICOMULTILOADER_MAGIC     0x10ADE125u

static inline void picomultiloader_reboot_to_menu(void)
{
    REG_WRITE(PICOMULTILOADER_MAGIC_REG, PICOMULTILOADER_MAGIC);
    esp_restart();
}

// An exit code for the loader: shown when its menu opens next. Then call
// picomultiloader_reboot_to_menu() (or restart some other way).
#define PICOMULTILOADER_EXIT_MAGIC_REG LP_SYSTEM_REG_LP_STORE13_REG
#define PICOMULTILOADER_EXIT_CODE_REG  LP_SYSTEM_REG_LP_STORE14_REG
#define PICOMULTILOADER_EXIT_MAGIC     0x504D4C58u // "PMLX"

static inline void picomultiloader_exit_code(int32_t code)
{
    REG_WRITE(PICOMULTILOADER_EXIT_CODE_REG, (uint32_t)code);
    REG_WRITE(PICOMULTILOADER_EXIT_MAGIC_REG, PICOMULTILOADER_EXIT_MAGIC);
}

// Names from before the project was called PicoMultiLoader, so older apps still build.
#define BINLOADER_MAGIC_REG      PICOMULTILOADER_MAGIC_REG
#define BINLOADER_MAGIC          PICOMULTILOADER_MAGIC
#define binloader_reboot_to_menu picomultiloader_reboot_to_menu
