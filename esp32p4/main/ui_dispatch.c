// ui_dispatch.c - the ESP32-P4 carries both menus, common/pml/ui.c and ui_classic.c, built with
// their ui_* functions renamed gui_* and uic_* (CMakeLists.txt): the "ui" setting picks one at
// switch-on. Each menu's Menu style row saves the other and restarts.
#include <string.h>
#include "config.h"
#include "ui.h"

void gui_boot(bool sd_ok);
void gui_boot_check(const char *what, const char *result, bool ok);
void gui_init(bool sd_ok);
void gui_key(int key);
void gui_redraw(void);
void gui_tick(void);
void gui_set_battery(int percent, bool charging);
uint8_t gui_backlight(void);

void uic_boot(bool sd_ok);
void uic_boot_check(const char *what, const char *result, bool ok);
void uic_init(bool sd_ok);
void uic_key(int key);
void uic_redraw(void);
void uic_tick(void);
void uic_set_battery(int percent, bool charging);
uint8_t uic_backlight(void);

static int s_classic = -1; // -1 = not read from the settings yet

static bool classic(void)
{
    if (s_classic < 0) {
        char buf[12];
        s_classic = config_get("ui", buf, sizeof(buf)) && strcmp(buf, "classic") == 0;
    }
    return s_classic;
}

void ui_boot(bool sd_ok)
{
    classic() ? uic_boot(sd_ok) : gui_boot(sd_ok);
}

void ui_boot_check(const char *what, const char *result, bool ok)
{
    classic() ? uic_boot_check(what, result, ok) : gui_boot_check(what, result, ok);
}

void ui_init(bool sd_ok)
{
    classic() ? uic_init(sd_ok) : gui_init(sd_ok);
}

void ui_key(int key)
{
    classic() ? uic_key(key) : gui_key(key);
}

void ui_redraw(void)
{
    classic() ? uic_redraw() : gui_redraw();
}

void ui_tick(void)
{
    classic() ? uic_tick() : gui_tick();
}

void ui_set_battery(int percent, bool charging)
{
    classic() ? uic_set_battery(percent, charging) : gui_set_battery(percent, charging);
}

uint8_t ui_backlight(void)
{
    return classic() ? uic_backlight() : gui_backlight();
}
