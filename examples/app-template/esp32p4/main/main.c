// main.c - PicoCalc start kit demo: four pages of text and graphics.
//
//   F1 Text    fonts, colours, scaled text and a line you can type in
//   F2 Shapes  lines, rectangles, circles, triangles and a gradient
//   F3 Bounce  animated balls (Up/Down: more or fewer), with the frame rate
//   F4 Chart   an animated line plot and bar chart (Up/Down: frequency)
//
// Left/Right also change the page. Esc returns to PicoMultiLoader's menu.
// This file is the same on the RP2350 and the ESP32-P4.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "gfx.h"
#include "picocalc.h"

#ifndef APP_NAME // set by the build from app.ini
#define APP_NAME "Start Kit"
#endif
#ifndef APP_VERSION
#define APP_VERSION "0.0.0"
#endif

#define PI 3.14159265f

// ---- the look ----------------------------------------------------------------------------
#define COL_BG     RGB(16, 20, 32)
#define COL_PANEL  RGB(23, 32, 48)
#define COL_BAR    RGB(33, 44, 64)
#define COL_LINE   RGB(51, 65, 92)
#define COL_FG     RGB(213, 221, 235)
#define COL_DIM    RGB(122, 136, 163)
#define COL_ACCENT RGB(76, 195, 255)

#define BAR_H  18
#define BODY_Y BAR_H
#define BODY_H (PC_H - 2 * BAR_H)

enum { PAGE_TEXT, PAGE_SHAPES, PAGE_BOUNCE, PAGE_CHART, PAGE_COUNT };
static const char *const kPageName[PAGE_COUNT] = {"Text", "Shapes", "Bounce", "Chart"};
static const char *const kPageHelp[PAGE_COUNT] = {
    "Type!  Enter clear",
    "",
    "Up/Dn balls",
    "Up/Dn freq",
};

static int s_battery = -1;
static bool s_charging;
static int s_fps;

static void draw_header(int page)
{
    gfx_fill_rect(0, 0, PC_W, BAR_H, COL_BAR);
    gfx_text(5, 3, APP_NAME, COL_FG, FONT_LARGE, 1);
    int x = 88;
    for (int i = 0; i < PAGE_COUNT; i++) {
        bool on = i == page;
        if (on) {
            gfx_fill_round_rect(x, 2, 56, BAR_H - 4, 4, COL_ACCENT);
        }
        char label[16];
        snprintf(label, sizeof(label), "F%d %s", i + 1, kPageName[i]);
        int tx = x + (56 - gfx_text_width(label, FONT_SMALL, 1)) / 2;
        gfx_text(tx, 5, label, on ? COL_BG : COL_DIM, FONT_SMALL, 1);
        x += 58;
    }
    gfx_hline(0, BAR_H - 1, PC_W, COL_LINE);
}

static void draw_footer(int page)
{
    int y = PC_H - BAR_H;
    gfx_fill_rect(0, y, PC_W, BAR_H, COL_BAR);
    gfx_hline(0, y, PC_W, COL_LINE);
    int x = gfx_text(5, y + 5, "</> page  Esc menu", COL_DIM, FONT_SMALL, 1);
    if (kPageHelp[page][0]) {
        gfx_text(x + 12, y + 5, kPageHelp[page], COL_FG, FONT_SMALL, 1);
    }

    // battery: an outline with a fill bar and the percentage
    int bx = PC_W - 62, by = y + 4;
    if (s_battery >= 0) {
        uint16_t c = s_battery > 50 ? C_GREEN : s_battery > 20 ? C_YELLOW : C_RED;
        gfx_rect(bx, by, 22, 10, COL_DIM);
        gfx_fill_rect(bx + 22, by + 3, 2, 4, COL_DIM);
        gfx_fill_rect(bx + 2, by + 2, 18 * s_battery / 100, 6, c);
        gfx_printf(bx + 28, y + 5, COL_FG, FONT_SMALL, 1, "%d%%%s", s_battery, s_charging ? "+" : "");
    } else {
        gfx_text(bx + 10, y + 5, "--", COL_DIM, FONT_SMALL, 1);
    }
}

// ---- F1: text ----------------------------------------------------------------------------

static char s_input[41];
static int s_input_len;

static void text_key(int key)
{
    if (key >= 0x20 && key <= 0x7E && s_input_len < (int)sizeof(s_input) - 1) {
        s_input[s_input_len++] = (char)key;
    } else if (key == KEY_BACKSPACE && s_input_len > 0) {
        s_input_len--;
    } else if (key == KEY_ENTER) {
        s_input_len = 0;
    }
    s_input[s_input_len] = 0;
}

static void text_draw(uint32_t now)
{
    int y = BODY_Y + 8;
    gfx_text(10, y, "Small font, 6x10 pixels:", COL_DIM, FONT_SMALL, 1);
    gfx_text(10, y + 12, "The quick brown fox jumps over the lazy dog.", COL_FG, FONT_SMALL, 1);
    y += 30;
    gfx_text(10, y, "Large font, 7x13 pixels:", COL_DIM, FONT_SMALL, 1);
    gfx_text(10, y + 12, "0123456789 !@#$%&*()[]{}<>", COL_FG, FONT_LARGE, 1);
    y += 34;

    // Scaled text with a drop shadow
    const char *hello = "Hello, PicoCalc!";
    int w = gfx_text_width(hello, FONT_LARGE, 2);
    gfx_text((PC_W - w) / 2 + 2, y + 2, hello, C_BLACK, FONT_LARGE, 2);
    gfx_text((PC_W - w) / 2, y, hello, COL_ACCENT, FONT_LARGE, 2);
    y += 36;

    // Colour swatches
    static const struct {
        uint16_t c;
        const char *name;
    } kSwatch[] = {
        {C_RED, "red"}, {C_ORANGE, "orange"}, {C_YELLOW, "yellow"}, {C_GREEN, "green"},
        {C_CYAN, "cyan"}, {C_BLUE, "blue"}, {C_MAGENTA, "magenta"}, {C_WHITE, "white"},
    };
    for (int i = 0; i < 8; i++) {
        int x = 10 + (i % 4) * 76, yy = y + (i / 4) * 18;
        gfx_fill_round_rect(x, yy, 12, 12, 3, kSwatch[i].c);
        gfx_text(x + 16, yy + 2, kSwatch[i].name, kSwatch[i].c, FONT_SMALL, 1);
    }
    y += 44;

    // A line to type in, with a blinking cursor
    gfx_text(10, y, "Type something:", COL_DIM, FONT_SMALL, 1);
    y += 13;
    gfx_fill_round_rect(10, y, PC_W - 20, 22, 4, COL_PANEL);
    gfx_rect(10, y, PC_W - 20, 22, COL_LINE);
    int cx = gfx_text(16, y + 5, s_input, COL_FG, FONT_LARGE, 1);
    if ((now / 500) % 2 == 0) {
        gfx_fill_rect(cx + 1, y + 4, 2, 14, COL_ACCENT);
    }
    y += 32;

    uint32_t s = now / 1000;
    gfx_printf(10, y, COL_DIM, FONT_SMALL, 1, "Board %s   version %s   up %lu:%02lu", pc_board_name(),
               APP_VERSION, (unsigned long)(s / 60), (unsigned long)(s % 60));
}

// ---- F2: shapes --------------------------------------------------------------------------

static void shapes_draw(uint32_t now)
{
    int top = BODY_Y + 6;

    // A spinning star of lines
    int cx = 60, cy = top + 55;
    float a0 = now / 1000.0f;
    for (int i = 0; i < 24; i++) {
        float a = a0 + i * (2 * PI / 24);
        uint16_t c = gfx_blend(C_CYAN, C_MAGENTA, i * 255 / 23);
        gfx_line(cx, cy, cx + (int)(48 * cosf(a)), cy + (int)(48 * sinf(a)), c);
    }
    gfx_text(cx - 15, top + 112, "lines", COL_DIM, FONT_SMALL, 1);

    // Rectangles: outlines, fills, rounded
    int x = 125;
    gfx_rect(x, top + 8, 70, 40, C_YELLOW);
    gfx_fill_rect(x + 8, top + 16, 54, 24, C_ORANGE);
    gfx_fill_round_rect(x, top + 58, 70, 40, 10, C_GREEN);
    gfx_text(x + 8, top + 72, "rounded", COL_BG, FONT_SMALL, 1);
    gfx_text(x + 12, top + 112, "rects", COL_DIM, FONT_SMALL, 1);

    // Circles
    x = 255;
    gfx_fill_circle(x, top + 50, 40, C_BLUE);
    gfx_fill_circle(x - 12, top + 38, 14, gfx_blend(C_BLUE, C_WHITE, 140));
    for (int r = 6; r <= 46; r += 10) {
        gfx_circle(x, top + 50, r, COL_LINE);
    }
    gfx_text(x - 21, top + 112, "circles", COL_DIM, FONT_SMALL, 1);

    // Triangles
    top += 130;
    gfx_fill_triangle(20, top + 70, 60, top, 100, top + 70, C_RED);
    gfx_fill_triangle(70, top + 70, 110, top + 15, 150, top + 70, gfx_blend(C_RED, C_YELLOW, 160));
    gfx_text(55, top + 76, "triangles", COL_DIM, FONT_SMALL, 1);

    // A pulsing filled circle
    int r = 18 + (int)(10 * sinf(now / 300.0f));
    gfx_fill_circle(235, top + 35, r, C_MAGENTA);
    gfx_circle(235, top + 35, 30, COL_FG);
    gfx_text(215, top + 76, "pulse", COL_DIM, FONT_SMALL, 1);

    // A horizontal gradient through a hue wheel
    top += 98;
    for (int i = 0; i < PC_W - 20; i++) {
        float h = i * 6.0f / (PC_W - 20); // 0..6, the six sides of the hue hexagon
        int seg = (int)h;
        int f = (int)((h - seg) * 255);
        int r8 = 0, g8 = 0, b8 = 0;
        switch (seg) {
        case 0: r8 = 255; g8 = f; break;
        case 1: r8 = 255 - f; g8 = 255; break;
        case 2: g8 = 255; b8 = f; break;
        case 3: g8 = 255 - f; b8 = 255; break;
        case 4: b8 = 255; r8 = f; break;
        default: b8 = 255 - f; r8 = 255; break;
        }
        gfx_vline(10 + i, top, 20, RGB(r8, g8, b8));
    }
    gfx_rect(9, top - 1, PC_W - 18, 22, COL_LINE);
}

// ---- F3: bouncing balls ------------------------------------------------------------------

#define MAX_BALLS 40

typedef struct {
    float x, y, vx, vy;
    int r;
    uint16_t c;
} ball_t;

static ball_t s_balls[MAX_BALLS];
static int s_nballs = 12;
static uint32_t s_rand = 12345;

static int rnd(int n)
{
    s_rand = s_rand * 1103515245u + 12345u;
    return (int)((s_rand >> 16) % (uint32_t)n);
}

static void ball_new(ball_t *b)
{
    static const uint16_t kColours[] = {C_RED, C_ORANGE, C_YELLOW, C_GREEN, C_CYAN, C_BLUE, C_MAGENTA};
    b->r = 6 + rnd(12);
    b->x = (float)(b->r + rnd(PC_W - 2 * b->r));
    b->y = (float)(BODY_Y + b->r + rnd(BODY_H - 2 * b->r));
    b->vx = (float)(rnd(200) - 100);
    b->vy = (float)(rnd(200) - 100);
    b->c = kColours[rnd(7)];
}

static void bounce_init(void)
{
    for (int i = 0; i < MAX_BALLS; i++) {
        ball_new(&s_balls[i]);
    }
}

static void bounce_key(int key)
{
    if (key == KEY_UP && s_nballs < MAX_BALLS) {
        s_nballs++;
    } else if (key == KEY_DOWN && s_nballs > 1) {
        s_nballs--;
    }
}

static void bounce_draw(float dt)
{
    const float gravity = 300.0f; // pixels/s^2
    for (int i = 0; i < s_nballs; i++) {
        ball_t *b = &s_balls[i];
        b->vy += gravity * dt;
        b->x += b->vx * dt;
        b->y += b->vy * dt;
        if (b->x < b->r) {
            b->x = (float)b->r;
            b->vx = -b->vx;
        } else if (b->x > PC_W - b->r) {
            b->x = (float)(PC_W - b->r);
            b->vx = -b->vx;
        }
        if (b->y > BODY_Y + BODY_H - b->r) {
            b->y = (float)(BODY_Y + BODY_H - b->r);
            b->vy = -b->vy * 0.98f;
            if (b->vy > -120.0f) { // keep them bouncing
                b->vy = -260.0f - rnd(120);
            }
        } else if (b->y < BODY_Y + b->r) {
            b->y = (float)(BODY_Y + b->r);
            b->vy = -b->vy;
        }
    }

    // floor grid
    for (int x = 0; x < PC_W; x += 20) {
        gfx_vline(x, BODY_Y, BODY_H, COL_PANEL);
    }
    for (int y = BODY_Y; y < BODY_Y + BODY_H; y += 20) {
        gfx_hline(0, y, PC_W, COL_PANEL);
    }

    for (int i = 0; i < s_nballs; i++) {
        ball_t *b = &s_balls[i];
        int x = (int)b->x, y = (int)b->y;
        gfx_fill_circle(x + 3, y + 3, b->r, RGB(8, 10, 16)); // shadow
        gfx_fill_circle(x, y, b->r, b->c);
        gfx_fill_circle(x - b->r / 3, y - b->r / 3, b->r / 3, gfx_blend(b->c, C_WHITE, 150));
    }

    gfx_fill_round_rect(PC_W - 112, BODY_Y + 6, 106, 18, 4, COL_BAR);
    gfx_printf(PC_W - 106, BODY_Y + 10, COL_FG, FONT_SMALL, 1, "%2d balls %2d fps", s_nballs, s_fps);
}

// ---- F4: chart ---------------------------------------------------------------------------

static int s_freq = 2;

static void chart_key(int key)
{
    if (key == KEY_UP && s_freq < 8) {
        s_freq++;
    } else if (key == KEY_DOWN && s_freq > 1) {
        s_freq--;
    }
}

static void chart_draw(uint32_t now)
{
    float t = now / 1000.0f;

    // Line plot: axes, grid, two curves, legend
    const int px = 34, py = BODY_Y + 10, pw = PC_W - px - 10, ph = 130;
    gfx_fill_rect(px, py, pw, ph, COL_PANEL);
    for (int i = 0; i <= 4; i++) {
        int y = py + i * ph / 4;
        gfx_hline(px, y, pw, COL_LINE);
        gfx_printf(4, y - 4, COL_DIM, FONT_SMALL, 1, "%4.1f", 1.0f - i * 0.5f);
    }
    for (int i = 0; i <= 8; i++) {
        gfx_vline(px + i * pw / 8, py, ph, COL_LINE);
    }
    int prev1 = 0, prev2 = 0;
    for (int i = 0; i < pw; i++) {
        float x = i * 2 * PI / pw;
        float v1 = sinf(s_freq * x - 2 * t);
        float v2 = 0.5f * cosf(s_freq * 2 * x + 3 * t) * sinf(t);
        int y1 = py + ph / 2 - (int)(v1 * (ph / 2 - 2));
        int y2 = py + ph / 2 - (int)(v2 * (ph / 2 - 2));
        if (i > 0) {
            gfx_line(px + i - 1, prev1, px + i, y1, C_CYAN);
            gfx_line(px + i - 1, prev2, px + i, y2, C_ORANGE);
        }
        prev1 = y1;
        prev2 = y2;
    }
    int ly = py + ph + 6;
    gfx_fill_rect(px, ly + 3, 12, 3, C_CYAN);
    gfx_printf(px + 16, ly, COL_FG, FONT_SMALL, 1, "sin(%dx - 2t)", s_freq);
    gfx_fill_rect(px + 130, ly + 3, 12, 3, C_ORANGE);
    gfx_text(px + 146, ly, "cos * sin(t)", COL_FG, FONT_SMALL, 1);

    // Bar chart: eight bars that rise and fall
    const int by = ly + 22, bh = 90, bw = 28;
    gfx_hline(px, by + bh, pw, COL_DIM);
    for (int i = 0; i < 8; i++) {
        float v = 0.55f + 0.45f * sinf(t * (1.0f + i * 0.3f) + i);
        int h = (int)(v * (bh - 12));
        int x = px + 6 + i * (bw + 6);
        uint16_t c = gfx_blend(C_GREEN, C_BLUE, i * 255 / 7);
        gfx_fill_rect(x, by + bh - h, bw, h, c);
        gfx_fill_rect(x, by + bh - h, bw, 3, gfx_blend(c, C_WHITE, 120));
        gfx_printf(x + 8, by + bh - h - 11, COL_FG, FONT_SMALL, 1, "%2d", (int)(v * 99));
    }
}

// ---- main loop ---------------------------------------------------------------------------

#ifdef ESP_PLATFORM
void app_main(void)
#else
int main(void)
#endif
{
    if (!pc_init()) {
        for (;;) {
            pc_sleep_ms(1000); // no screen: nothing to show
        }
    }
    gfx_init(pc_framebuffer(), PC_W, PC_H);
    bounce_init();

    int page = PAGE_TEXT;
    uint32_t last = pc_millis(), fps_start = last, next_battery = 0;
    int frames = 0;

    for (;;) {
        // Input
        int key;
        while ((key = pc_key()) != KEY_NONE) {
            if (key == KEY_ESC) {
                gfx_clear(COL_BG);
                gfx_text(90, 150, "Back to the menu...", COL_FG, FONT_LARGE, 1);
                pc_flush();
                pc_exit_to_menu();
            } else if (key >= KEY_F1 && key < KEY_F1 + PAGE_COUNT) {
                page = key - KEY_F1;
            } else if (key == KEY_RIGHT) {
                page = (page + 1) % PAGE_COUNT;
            } else if (key == KEY_LEFT) {
                page = (page + PAGE_COUNT - 1) % PAGE_COUNT;
            } else if (page == PAGE_TEXT) {
                text_key(key);
            } else if (page == PAGE_BOUNCE) {
                bounce_key(key);
            } else if (page == PAGE_CHART) {
                chart_key(key);
            }
        }

        // Time
        uint32_t now = pc_millis();
        float dt = (now - last) / 1000.0f;
        if (dt > 0.1f) {
            dt = 0.1f;
        }
        last = now;
        if (now >= next_battery) { // the keyboard's I2C is slow: don't ask every frame
            s_battery = pc_battery(&s_charging);
            next_battery = now + 5000;
        }

        // Draw
        gfx_clear(COL_BG);
        gfx_clip(0, BODY_Y, PC_W, BODY_H);
        switch (page) {
        case PAGE_TEXT: text_draw(now); break;
        case PAGE_SHAPES: shapes_draw(now); break;
        case PAGE_BOUNCE: bounce_draw(dt); break;
        case PAGE_CHART: chart_draw(now); break;
        }
        gfx_noclip();
        draw_header(page);
        draw_footer(page);
        pc_flush();

        frames++;
        if (now - fps_start >= 1000) {
            s_fps = frames * 1000 / (int)(now - fps_start);
            frames = 0;
            fps_start = now;
        }
        pc_sleep_ms(1); // let other tasks run (ESP-IDF's idle task and watchdog)
    }
}
