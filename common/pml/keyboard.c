// keyboard.c - the PicoCalc's STM32 keyboard controller (keyboard.h): key events, Ctrl, auto-repeat
// and the battery. The platform's kbd_bus.c does the I2C (kbd_bus_*).
#include "keyboard.h"

#include "plat.h"

#define REG_FIFO  0x09 // key event FIFO: {state, key}
#define REG_BK2   0x0A // LCD backlight
#define REG_BAT   0x0B // battery: bits 0..6 percent, bit 7 charging

#define STATE_PRESSED  1
#define STATE_HOLD     2
#define STATE_RELEASED 3

#define REPEAT_DELAY_US  400000
#define REPEAT_PERIOD_US 70000

static bool s_inited;
static bool s_present;
static int s_failures;
static int s_held_key;
static uint64_t s_next_repeat;

// Treat the keyboard as gone (PicoCalc switched off) only after a few failures in a row.
static void note_result(bool ok)
{
    if (ok) {
        s_failures = 0;
        s_present = true;
    } else if (++s_failures >= 3) {
        s_present = false;
    }
}

static bool kbd_read(uint8_t reg, uint8_t out[2])
{
    bool ok = kbd_bus_read(reg, out);
    note_result(ok);
    return ok;
}

static void kbd_write(uint8_t reg, uint8_t value)
{
    note_result(kbd_bus_write(reg, value));
}

static void bus_init(void)
{
    if (!s_inited) {
        kbd_bus_init();
        s_inited = true;
    }
}

bool kbd_init(void)
{
    bus_init();
    uint8_t r[2];
    s_failures = 0;
    s_present = kbd_read(REG_FIFO, r);
    if (s_present) {
        // Drain events queued before we started (e.g. the key that opened the loader)
        for (int i = 0; i < 32 && kbd_read(REG_FIFO, r) && r[1] != 0; i++) {
        }
    }
    return s_present;
}

bool kbd_present(void)
{
    return s_present;
}

static bool is_repeatable(int key)
{
    return key == KEY_UP || key == KEY_DOWN || key == KEY_PAGE_UP || key == KEY_PAGE_DOWN;
}

#define KEY_MOD_CTRL 0xA5
static bool s_ctrl;      // Ctrl is held
static uint8_t s_ahead[2]; // an event kbd_waiting() read, for the next kbd_poll()
static bool s_have_ahead;

static bool next_event(uint8_t r[2])
{
    if (s_have_ahead) {
        s_have_ahead = false;
        r[0] = s_ahead[0];
        r[1] = s_ahead[1];
        return true;
    }
    return kbd_read(REG_FIFO, r) && r[1] != 0;
}

bool kbd_waiting(void)
{
    if (!s_inited) {
        return false;
    }
    if (s_have_ahead || (s_held_key != KEY_NONE && time_us_64() >= s_next_repeat)) {
        return true;
    }
    s_have_ahead = kbd_read(REG_FIFO, s_ahead) && s_ahead[1] != 0;
    return s_have_ahead;
}

int kbd_poll(void)
{
    if (!s_inited) {
        return KEY_NONE;
    }
    uint8_t r[2];
    if (next_event(r)) {
        int key = r[1];
        if (key == KEY_MOD_CTRL) { // a modifier: held while the letter is pressed
            s_ctrl = r[0] == STATE_PRESSED || (r[0] != STATE_RELEASED && s_ctrl);
            return KEY_NONE;
        }
        if (s_ctrl && ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z'))) {
            key &= 0x1F;
        }
        if (r[0] == STATE_PRESSED) {
            s_held_key = is_repeatable(key) ? key : KEY_NONE;
            s_next_repeat = time_us_64() + REPEAT_DELAY_US;
            return key;
        }
        if (r[0] == STATE_RELEASED && key == s_held_key) {
            s_held_key = KEY_NONE;
        }
        return KEY_NONE;
    }
    if (s_held_key != KEY_NONE && time_us_64() >= s_next_repeat) {
        s_next_repeat = time_us_64() + REPEAT_PERIOD_US;
        return s_held_key;
    }
    return KEY_NONE;
}

int kbd_battery(bool *charging)
{
    uint8_t r[2];
    if (!s_inited || !kbd_read(REG_BAT, r)) {
        return -1;
    }
    if (charging) {
        *charging = (r[1] & 0x80) != 0;
    }
    int pct = r[1] & 0x7F;
    return pct > 100 ? 100 : pct;
}

void kbd_set_lcd_backlight(uint8_t level)
{
    if (s_inited) {
        kbd_write(REG_BK2, level);
    }
}

bool kbd_boot_key_held(uint32_t window_ms)
{
    bus_init();
    uint64_t end = time_us_64() + (uint64_t)window_ms * 1000;
    while (time_us_64() < end) {
        uint8_t r[2];
        if (kbd_read(REG_FIFO, r) && r[1] != 0) {
            if ((r[0] == STATE_PRESSED || r[0] == STATE_HOLD) && (r[1] == KEY_UP || r[1] == KEY_F1 || r[1] == KEY_F5)) {
                return true;
            }
            continue; // more events may be queued
        }
        sleep_ms(10);
    }
    return false;
}
