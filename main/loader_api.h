// loader_api.h - copy this header into your own ESP32-P4 app to return to the bin loader in software
// (the equivalent of the PicoCalc UF2 Loader's "watchdog scratch" command).
//
//     #include "loader_api.h"
//     binloader_reboot_to_menu();
//
// It leaves a magic value in an LP_SYSTEM store register, which survives a software reset, and
// restarts. The bin loader's custom bootloader sees it and boots the loader instead of the app.
// Without the bin loader installed, this is a plain restart.
#pragma once

#include "esp_system.h"
#include "soc/soc.h"
#include "soc/lp_system_reg.h"

#define BINLOADER_MAGIC_REG LP_SYSTEM_REG_LP_STORE15_REG
#define BINLOADER_MAGIC     0x10ADE125u

static inline void binloader_reboot_to_menu(void)
{
    REG_WRITE(BINLOADER_MAGIC_REG, BINLOADER_MAGIC);
    esp_restart();
}
