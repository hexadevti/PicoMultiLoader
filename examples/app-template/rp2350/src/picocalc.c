// picocalc.c - the board-independent part of picocalc.h: the frame buffer and the keyboard logic.
// The STM32 keyboard controller at I2C address 0x1F reports events from a FIFO register; the
// platform port does the I2C.
#include "picocalc.h"

#define REG_FIFO 0x09 // key event FIFO: {state, key}
#define REG_BKL  0x0A // LCD backlight
#define REG_BAT  0x0B // battery: bits 0..6 percent, bit 7 charging

#define STATE_PRESSED  1
#define STATE_HOLD     2
#define STATE_RELEASED 3

#define REPEAT_DELAY_US  400000
#define REPEAT_PERIOD_US 70000

static uint16_t s_fb[PC_W * PC_H];

static int s_held_key;
static uint64_t s_next_repeat;

bool pc_init(void)
{
    port_kbd_init();
    // Drop the events queued before the app started (e.g. the Enter that started it).
    uint8_t r[2];
    for (int i = 0; i < 32 && port_kbd_read(REG_FIFO, r) && r[1] != 0; i++) {
    }
    return port_lcd_init();
}

uint16_t *pc_framebuffer(void)
{
    return s_fb;
}

void pc_flush(void)
{
    port_lcd_send(s_fb);
}

static bool repeats(int key)
{
    return key == KEY_UP || key == KEY_DOWN || key == KEY_LEFT || key == KEY_RIGHT ||
           key == KEY_BACKSPACE || key == KEY_DEL || key == KEY_PAGE_UP || key == KEY_PAGE_DOWN;
}

int pc_key(void)
{
    uint8_t r[2];
    while (port_kbd_read(REG_FIFO, r) && r[1] != 0) {
        int key = r[1];
        if (r[0] == STATE_PRESSED) {
            s_held_key = repeats(key) ? key : KEY_NONE;
            s_next_repeat = port_time_us() + REPEAT_DELAY_US;
            return key;
        }
        if (r[0] == STATE_RELEASED && key == s_held_key) {
            s_held_key = KEY_NONE;
        }
        // STATE_HOLD and other releases: nothing to report, look at the next event
    }
    if (s_held_key != KEY_NONE && port_time_us() >= s_next_repeat) {
        s_next_repeat = port_time_us() + REPEAT_PERIOD_US;
        return s_held_key;
    }
    return KEY_NONE;
}

int pc_battery(bool *charging)
{
    uint8_t r[2];
    if (!port_kbd_read(REG_BAT, r)) {
        return -1;
    }
    if (charging) {
        *charging = (r[1] & 0x80) != 0;
    }
    int pct = r[1] & 0x7F;
    return pct > 100 ? 100 : pct;
}

void pc_backlight(uint8_t level)
{
    port_kbd_write(REG_BKL, level);
}

uint32_t pc_millis(void)
{
    return (uint32_t)(port_time_us() / 1000);
}

void pc_sleep_ms(uint32_t ms)
{
    port_sleep_ms(ms);
}

const char *pc_board_name(void)
{
    return port_board_name();
}

void pc_exit_to_menu(void)
{
    port_exit_to_menu();
}
