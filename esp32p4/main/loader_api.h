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

// Names from before the project was called PicoMultiLoader, so older apps still build.
#define BINLOADER_MAGIC_REG      PICOMULTILOADER_MAGIC_REG
#define BINLOADER_MAGIC          PICOMULTILOADER_MAGIC
#define binloader_reboot_to_menu picomultiloader_reboot_to_menu
