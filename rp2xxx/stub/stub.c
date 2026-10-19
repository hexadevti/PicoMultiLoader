// stub.c - PicoMultiLoader boot stub for the RP2350 (the only part of the loader kept in flash).
//
// It occupies the first 8 KB of flash and carries a partition table that gives apps the rest of
// the chip (0x2000 to the end), the same layout as pelrun's uf2loader, so apps made for that work
// unchanged. At reset it either starts the app or loads the full loader (PML2350.UF2 in the root of
// the SD card) into RAM and runs it:
//
//   - an app asked for it the uf2loader way (watchdog scratch): mode 0 = app, otherwise the menu;
//   - otherwise Up, F1 or F5 held on the keyboard in the first 0.5 s shows the menu;
//   - otherwise the app starts. If there is no valid app the menu shows, and if the loader file
//     can't be loaded either, the chip drops into BOOTSEL (USB drive) so it can be fixed.
//
// The stub's own RAM is confined to the scratch banks, leaving main RAM free for the loader.
#include <string.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/resets.h"
#include "hardware/spi.h"
#include "hardware/sync.h"
#include "hardware/structs/watchdog.h"
#include "board.h"

#define APP_OFFSET 0x2000u
#define APP_SIZE   (PICO_FLASH_SIZE_BYTES - APP_OFFSET)

#define BL_MAGIC        0xe98cc638u // uf2loader: scratch[0]; scratch[1] = mode
#define BL_MODE_DEFAULT 0           // start the app

#define LOADER_NAME  "PML2350 UF2" // 8.3 directory entry name
#define LOADER_RAM   0x20000000u
#define LOADER_END   0x20080000u   // main RAM; the scratch banks hold the stub
#define RP2350_ARM_S 0xe48bff59u

// ---- keyboard ---------------------------------------------------------------------------------
static bool menu_key_held(uint32_t window_ms)
{
    i2c_init(KBD_I2C, KBD_I2C_HZ);
    gpio_set_function(KBD_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(KBD_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(KBD_SDA_PIN);
    gpio_pull_up(KBD_SCL_PIN);
    absolute_time_t end = make_timeout_time_ms(window_ms);
    while (!time_reached(end)) {
        uint8_t reg = 0x09, r[2]; // key FIFO: {state, key}
        if (i2c_write_timeout_us(KBD_I2C, KBD_I2C_ADDR, &reg, 1, false, 50000) == 1) {
            sleep_us(2000);
            if (i2c_read_timeout_us(KBD_I2C, KBD_I2C_ADDR, r, 2, false, 50000) == 2 && r[1] != 0) {
                if ((r[0] == 1 || r[0] == 2) && (r[1] == 0xB5 || r[1] == 0x81 || r[1] == 0x85)) {
                    return true; // Up, F1, F5 pressed or held
                }
                continue;
            }
        }
        sleep_ms(10);
    }
    return false;
}

// ---- SD card (SPI) ----------------------------------------------------------------------------
static bool s_block_addr;

static uint8_t xfer(uint8_t b)
{
    uint8_t r;
    spi_write_read_blocking(SD_SPI, &b, &r, 1);
    return r;
}

static void deselect(void)
{
    gpio_put(SD_CS_PIN, 1);
    xfer(0xFF);
}

static uint8_t cmd(uint8_t c, uint32_t arg)
{
    if (c & 0x80) { // ACMD
        uint8_t r = cmd(55, 0);
        if (r > 1) {
            return r;
        }
        c &= 0x7F;
    }
    deselect();
    gpio_put(SD_CS_PIN, 0);
    for (int i = 0; i < 50000 && xfer(0xFF) != 0xFF; i++) {
    }
    uint8_t f[6] = {(uint8_t)(0x40 | c), (uint8_t)(arg >> 24), (uint8_t)(arg >> 16), (uint8_t)(arg >> 8), (uint8_t)arg,
                    c == 0 ? 0x95 : c == 8 ? 0x87 : 0x01};
    spi_write_blocking(SD_SPI, f, 6);
    uint8_t r = 0xFF;
    for (int i = 0; i < 10 && ((r = xfer(0xFF)) & 0x80); i++) {
    }
    return r;
}

static bool sd_init(void)
{
    gpio_init(SD_DET_PIN);
    gpio_pull_up(SD_DET_PIN);
    gpio_init(SD_CS_PIN);
    gpio_set_dir(SD_CS_PIN, GPIO_OUT);
    gpio_put(SD_CS_PIN, 1);
    spi_init(SD_SPI, 400 * 1000);
    gpio_set_function(SD_SCK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(SD_MOSI_PIN, GPIO_FUNC_SPI);
    gpio_set_function(SD_MISO_PIN, GPIO_FUNC_SPI);
    gpio_pull_up(SD_MISO_PIN);
    sleep_ms(2);
    if (gpio_get(SD_DET_PIN)) {
        return false; // no card
    }
    for (int i = 0; i < 10; i++) {
        xfer(0xFF);
    }
    uint8_t r = 0xFF;
    for (int i = 0; i < 10 && r != 1; i++) {
        r = cmd(0, 0);
    }
    if (r != 1) {
        return false;
    }
    uint8_t ocr[4];
    bool v2 = cmd(8, 0x1AA) == 1;
    if (v2) {
        spi_read_blocking(SD_SPI, 0xFF, ocr, 4);
    }
    absolute_time_t until = make_timeout_time_ms(1000);
    while ((r = cmd(0x80 | 41, v2 ? 1u << 30 : 0)) == 1 && !time_reached(until)) {
    }
    if (r != 0) {
        return false;
    }
    s_block_addr = false;
    if (v2 && cmd(58, 0) == 0) {
        spi_read_blocking(SD_SPI, 0xFF, ocr, 4);
        s_block_addr = (ocr[0] & 0x40) != 0;
    }
    if (!s_block_addr && cmd(16, 512) != 0) {
        return false;
    }
    deselect();
    spi_set_baudrate(SD_SPI, SD_SPI_HZ);
    return true;
}

static uint8_t s_buf[512] __attribute__((aligned(4)));

static bool sd_read(uint32_t lba)
{
    bool ok = false;
    if (cmd(17, s_block_addr ? lba : lba * 512) == 0) {
        uint8_t t = 0xFF;
        for (int i = 0; i < 100000 && (t = xfer(0xFF)) == 0xFF; i++) {
        }
        if (t == 0xFE) {
            spi_read_blocking(SD_SPI, 0xFF, s_buf, 512);
            xfer(0xFF);
            xfer(0xFF);
            ok = true;
        }
    }
    deselect();
    return ok;
}

// ---- FAT16/FAT32, read-only, 8.3 names in the root directory ----------------------------------
static uint32_t s_fat, s_data, s_root, s_root_secs, s_spc;
static bool s_fat32;

static uint16_t u16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t u32(const uint8_t *p) { return u16(p) | (uint32_t)u16(p + 2) << 16; }

static bool is_vbr(void)
{
    return (s_buf[0] == 0xEB || s_buf[0] == 0xE9) && u16(s_buf + 11) == 512 && s_buf[13] && s_buf[16];
}

static bool fat_mount(void)
{
    uint32_t vol = 0;
    if (!sd_read(0) || u16(s_buf + 510) != 0xAA55) {
        return false;
    }
    if (!is_vbr()) { // MBR: first partition
        vol = u32(s_buf + 0x1BE + 8);
        if (!sd_read(vol) || !is_vbr()) {
            return false;
        }
    }
    uint32_t fatsz = u16(s_buf + 22) ? u16(s_buf + 22) : u32(s_buf + 36);
    s_spc = s_buf[13];
    s_fat = vol + u16(s_buf + 14);
    s_root = s_fat + s_buf[16] * fatsz;
    s_root_secs = (u16(s_buf + 17) * 32 + 511) / 512;
    s_fat32 = s_root_secs == 0;
    s_data = s_root + s_root_secs;
    if (s_fat32) {
        s_root = u32(s_buf + 44); // root directory cluster
    }
    return true;
}

static uint32_t next_cluster(uint32_t cl)
{
    uint32_t per = s_fat32 ? 128 : 256;
    if (!sd_read(s_fat + cl / per)) {
        return 0;
    }
    if (s_fat32) {
        cl = u32(s_buf + (cl % per) * 4) & 0x0FFFFFFF;
        return cl >= 0x0FFFFFF8 ? 0 : cl;
    }
    cl = u16(s_buf + (cl % per) * 2);
    return cl >= 0xFFF8 ? 0 : cl;
}

static uint32_t cluster_lba(uint32_t cl) { return s_data + (cl - 2) * s_spc; }

// Calls fn(index) for each sector of a cluster chain (in s_buf); fn returns false to stop.
// cl == 0 means the FAT16 root directory. Returns false on a read error.
typedef int (*sector_fn_t)(void);

static bool walk_chain(uint32_t cl, sector_fn_t fn)
{
    if (cl == 0) {
        for (uint32_t i = 0; i < s_root_secs; i++) {
            if (!sd_read(s_root + i)) {
                return false;
            }
            if (fn() <= 0) {
                return true;
            }
        }
        return true;
    }
    while (cl >= 2) {
        for (uint32_t i = 0; i < s_spc; i++) {
            if (!sd_read(cluster_lba(cl) + i)) {
                return false;
            }
            int r = fn();
            if (r <= 0) {
                return r == 0;
            }
        }
        cl = next_cluster(cl);
    }
    return true;
}

static uint32_t s_file_cl;

// Directory sector: >0 continue, 0 done (found or end of directory)
static int dir_sector(void)
{
    for (int i = 0; i < 512; i += 32) {
        const uint8_t *e = s_buf + i;
        if (e[0] == 0) {
            return 0;
        }
        if (e[0] != 0xE5 && !(e[11] & 0x18) && memcmp(e, LOADER_NAME, 11) == 0) {
            s_file_cl = (uint32_t)u16(e + 20) << 16 | u16(e + 26);
            return 0;
        }
    }
    return 1;
}

// ---- loading the loader -----------------------------------------------------------------------
static uint32_t s_loaded, s_expected;

// One file sector = one UF2 block: >0 continue, 0 all blocks loaded, <0 bad block
static int uf2_sector(void)
{
    const uint32_t *h = (const uint32_t *)s_buf;
    if (h[0] != 0x0A324655 || h[1] != 0x9E5D5157 || h[127] != 0x0AB16F30) {
        return -1;
    }
    uint32_t flags = h[2], addr = h[3], size = h[4], num = h[6], fam = h[7];
    if ((flags & 1) || !(flags & 0x2000) || fam != RP2350_ARM_S) {
        return 1; // not for us
    }
    if (size > 476 || addr < LOADER_RAM || addr + size > LOADER_END) {
        return -1;
    }
    memcpy((void *)addr, s_buf + 32, size);
    s_expected = num;
    return ++s_loaded == num ? 0 : 1;
}

static bool load_loader(void)
{
    if (!sd_init() || !fat_mount()) {
        return false;
    }
    s_file_cl = 0;
    if (!walk_chain(s_fat32 ? s_root : 0, dir_sector) || s_file_cl < 2) {
        return false;
    }
    s_loaded = 0;
    return walk_chain(s_file_cl, uf2_sector) && s_loaded && s_loaded == s_expected;
}

// ---- starting things --------------------------------------------------------------------------
// Put back what the stub touched, so whatever runs next sees the chip as after a reset.
static void tidy_up(void)
{
    reset_block(RESETS_RESET_I2C1_BITS | RESETS_RESET_SPI0_BITS);
    volatile uint32_t *icer = (volatile uint32_t *)(PPB_BASE + 0xE180);
    volatile uint32_t *icpr = (volatile uint32_t *)(PPB_BASE + 0xE280);
    *(volatile uint32_t *)(PPB_BASE + 0xE010) = 0; // SysTick off
    for (unsigned i = 0; i < (NUM_IRQS + 31) / 32; i++) {
        icer[i] = 0xffffffffu;
        icpr[i] = 0xffffffffu;
    }
}

// The bootrom checks the app's IMAGE_DEF, maps the app partition at XIP_BASE and enters it.
// Returns only if there is no valid app.
static void start_app(void)
{
    tidy_up();
    rom_chain_image_fn chain = (rom_chain_image_fn)rom_func_lookup(ROM_FUNC_CHAIN_IMAGE);
    bootrom_release_lock(BOOTROM_LOCK_ENABLE);
    chain((uint8_t *)LOADER_RAM, 4096, XIP_BASE + APP_OFFSET, APP_SIZE); // main RAM is free here
}

static void __attribute__((noreturn)) start_loader(void)
{
    tidy_up();
    const uint32_t *vt = (const uint32_t *)LOADER_RAM;
    *(volatile uint32_t *)(PPB_BASE + 0xED08) = LOADER_RAM; // VTOR
    __asm volatile("msr msp, %0\n"
                   "bx %1\n" ::"r"(vt[0]), "r"(vt[1]));
    __builtin_unreachable();
}

int main(void)
{
    uint32_t magic = watchdog_hw->scratch[0], mode = watchdog_hw->scratch[1];
    watchdog_hw->scratch[0] = 0;
    bool menu = magic == BL_MAGIC ? mode != BL_MODE_DEFAULT : menu_key_held(500);
    if (!menu) {
        start_app();
    }
    if (load_loader()) {
        start_loader();
    }
    tidy_up();
    reset_usb_boot(0, 0); // nothing to run: BOOTSEL, so files can be copied over USB
}
