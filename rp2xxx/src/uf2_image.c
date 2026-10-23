// uf2_image.c - UF2 checks, flashing, and starting the installed app.
//
// RP2040: PicoMultiLoader is the only firmware on the chip: it boots at power-on and either shows
// the menu or starts the app it flashed earlier.
// RP2350: a boot stub in the first 8 KB of flash (stub/stub.c) makes that choice and loads this
// loader from the SD card into RAM; apps live from 0x2000 to the end of flash, as with uf2loader.
// Either way apps can ask for the menu the way pelrun's uf2loader defines it (watchdog scratch
// request), so PicoCalc apps made for it work; no code is taken from it.
#include "uf2_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "pico/runtime.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include "boot/uf2.h"
#include "config.h"
#include "ff.h"
#include "keyboard.h"
#include "sdcard.h"

// ---- flash layout (see uf2_image.h) -----------------------------------------------------------
#if PICO_RP2040
#define APP_OFFSET    0x000000u // apps run where they are linked; our boot2 replaces theirs
#define RECORD_OFFSET 0x1BF000u
#define APP_SIZE      RECORD_OFFSET
#else
#define APP_OFFSET    0x002000u // the stub's partition; the bootrom maps it to XIP_BASE
#define RES_SIZE      0x40000u  // the loader's resources (Wi-Fi firmware, web page), at the end
#define RES_OFFSET    (PICO_FLASH_SIZE_BYTES - RES_SIZE)
#define APP_SIZE      (RES_OFFSET - APP_OFFSET)
#define APP_CONFIG_KEY "app"    // file name of the installed app, in the settings file
#endif
#define APP_END       (XIP_BASE + APP_SIZE) // end of the app's own address range

#if PICO_RP2040
// Written after an app is flashed and verified, erased before flashing starts.
#define RECORD_MAGIC 0x214c4d50u // "PML!"

typedef struct {
    uint32_t magic;
    uint32_t size;     // bytes from the start of the app
    uint32_t crc;      // CRC-32 of the app's first sector as it is in flash
    uint32_t check;    // magic ^ size ^ crc ^ 0xffffffff
    char file[64];     // file name it was flashed from
} app_record_t;
#endif

// ---- boot requests (watchdog scratch, kept across a watchdog reboot) --------------------------
#define BL_MAGIC  0xe98cc638u // uf2loader: scratch[0]; scratch[1] = mode
#define BL_MODE_DEFAULT 0     // uf2loader: start the app
#define PML_MAGIC 0x504d4c52u // ours: scratch[0]; scratch[1] = PML_RUN_APP
#define PML_RUN_APP 1

#define BI_TAG_RP              0x5052 // 'R','P'
#define BI_TYPE_ID_AND_STRING  6
#define BI_ID_PROGRAM_NAME     0x02031c86
#define BI_ID_PROGRAM_VERSION  0x11a9bc3a
#define BI_ID_PROGRAM_DATE     0x9da22254
#define BI_MARKER_START        0x7188ebf2
#define BI_MARKER_END          0xe71aa390

bool uf2_exit_code(int32_t *code)
{
    if (watchdog_hw->scratch[2] != PML_EXIT_MAGIC) {
        return false;
    }
    watchdog_hw->scratch[2] = 0;
    *code = (int32_t)watchdog_hw->scratch[3];
    return true;
}

// ---- platform ---------------------------------------------------------------------------------
uint32_t uf2_flash_capacity(void)
{
    return APP_SIZE;
}

const char *uf2_chip_name(void)
{
#if PICO_RP2040
    return "RP2040";
#else
    return "RP2350";
#endif
}

const char *uf2_family_name(uint32_t family)
{
    switch (family) {
    case RP2040_FAMILY_ID:
        return "RP2040";
    case RP2350_ARM_S_FAMILY_ID:
        return "RP2350 ARM";
    case RP2350_ARM_NS_FAMILY_ID:
        return "RP2350 ARM (non-secure)";
    case RP2350_RISCV_FAMILY_ID:
        return "RP2350 RISC-V";
    case ABSOLUTE_FAMILY_ID:
        return "absolute";
    case DATA_FAMILY_ID:
        return "data";
    default:
        return "unknown";
    }
}

static bool family_ok(uint32_t family)
{
#if PICO_RP2040
    return family == RP2040_FAMILY_ID;
#else
    return family == RP2350_ARM_S_FAMILY_ID || family == RP2350_ARM_NS_FAMILY_ID || family == RP2350_RISCV_FAMILY_ID;
#endif
}

static bool in_ram(uint32_t addr)
{
    return addr >= SRAM_BASE && addr + 256 <= SRAM_END;
}

static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xffffffffu;
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) {
            crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1));
        }
    }
    return ~crc;
}

#if PICO_RP2040
static uint32_t first_sector_crc(uint32_t size)
{
    return crc32((const uint8_t *)(XIP_BASE + APP_OFFSET), size < FLASH_SECTOR_SIZE ? size : FLASH_SECTOR_SIZE);
}

// The record, if it is valid and the app's first sector still matches it (re-installing the loader
// through BOOTSEL overwrites that sector on the RP2040).
static const app_record_t *installed_record(void)
{
    const app_record_t *r = (const app_record_t *)(XIP_BASE + RECORD_OFFSET);
    if (r->magic != RECORD_MAGIC || r->check != (r->magic ^ r->size ^ r->crc ^ 0xffffffffu) ||
        r->size == 0 || r->size > APP_SIZE || first_sector_crc(r->size) != r->crc) {
        return NULL;
    }
    return r;
}
#else
// The app is valid if its first 4 KB hold a picobin block (the IMAGE_DEF the bootrom looks for).
// That sector is written last when flashing, so a half-written app is never taken for one.
static bool app_present(void)
{
    const uint32_t *p = (const uint32_t *)(XIP_BASE + APP_OFFSET);
    for (unsigned i = 0; i < FLASH_SECTOR_SIZE / 4; i++) {
        if (p[i] == 0xffffded3u) { // PICOBIN_BLOCK_MARKER_START
            return true;
        }
    }
    return false;
}
#endif

#if PICO_RP2040
// ---- starting the app -------------------------------------------------------------------------
// Switch off every interrupt source the loader may have enabled; PRIMASK stays clear, as the app's
// startup code expects after a reset.
static void quiet_irqs(void)
{
    volatile uint32_t *icer = (volatile uint32_t *)(PPB_BASE + 0xE180); // NVIC_ICER0..
    volatile uint32_t *icpr = (volatile uint32_t *)(PPB_BASE + 0xE280); // NVIC_ICPR0..
    *(volatile uint32_t *)(PPB_BASE + 0xE010) = 0; // SysTick off
    for (unsigned i = 0; i < (NUM_IRQS + 31) / 32; i++) {
        icer[i] = 0xffffffffu;
        icpr[i] = 0xffffffffu;
    }
}

// Enter the app. Called from early_boot() only, so the chip is as the bootrom left it: no clocks,
// PLLs, flash timing or interrupts set up by the loader's own startup.
static void start_app(void)
{
    quiet_irqs();
    // Like boot2's exit: the app's vector table follows its 256-byte boot2.
    const uint32_t *vt = (const uint32_t *)(XIP_BASE + 0x100);
    *(volatile uint32_t *)(PPB_BASE + 0xED08) = (uint32_t)vt; // VTOR
    __asm volatile("msr msp, %0\n"
                   "bx %1\n" ::"r"(vt[0]), "r"(vt[1]));
    __builtin_unreachable();
}

// Runs before the rest of the SDK's startup (right after .data/.bss are set up). If the previous
// run asked for the app, start it now, from a freshly reset chip.
static void early_boot(void)
{
    if (watchdog_hw->scratch[0] != PML_MAGIC || watchdog_hw->scratch[1] != PML_RUN_APP) {
        return;
    }
    watchdog_hw->scratch[0] = 0; // the app's own reboots must not land here again
    if (installed_record()) {
        start_app();
    }
}
PICO_RUNTIME_INIT_FUNC(early_boot, "00001");

// Reboot so that early_boot() starts the app.
static void __attribute__((noreturn)) reboot_into_app(void)
{
    printf("boot: starting the app\n");
    stdio_flush();
    watchdog_hw->scratch[1] = PML_RUN_APP;
    watchdog_hw->scratch[0] = PML_MAGIC;
    watchdog_reboot(0, 0, 0);
    for (;;) {
        tight_loop_contents();
    }
}

void uf2_boot_or_menu(void)
{
    uint32_t magic = watchdog_hw->scratch[0], mode = watchdog_hw->scratch[1];
    watchdog_hw->scratch[0] = 0;
    if (magic == BL_MAGIC && mode == 1 && !installed_record()) {
        printf("boot: no app installed\n");
        return;
    }
    if (magic == BL_MAGIC && mode != BL_MODE_DEFAULT) {
        printf("boot: menu requested\n");
        return;
    }
    if (!installed_record()) {
        printf("boot: no app installed\n");
        return;
    }
    if (magic != BL_MAGIC && kbd_boot_key_held(500)) {
        printf("boot: menu key held\n");
        return;
    }
    reboot_into_app();
}
#else
// The boot stub makes the choice at reset; the loader only runs when the menu is wanted.
void uf2_boot_or_menu(void)
{
    watchdog_hw->scratch[0] = 0;
}
#endif

// ---- walking a UF2 file -----------------------------------------------------------------------
#define CHUNK_BLOCKS 4

typedef struct uf2_walk uf2_walk_t;
typedef bool (*block_fn_t)(uf2_walk_t *w, const struct uf2_block *b);

struct uf2_walk {
    FIL f;
    uf2_info_t *info;
    block_fn_t on_block; // called for each accepted block; false stops the walk
    uf2_progress_cb_t progress;
    char *err;
    size_t err_len;
    uint32_t first_index; // file block index of the first accepted block
    uint8_t erased[512];  // sector bitmap (flashing)
    uint32_t written;
    uint32_t first_pages; // RP2350: pages of the app's first sector, programmed last
};

static struct uf2_block s_chunk[CHUNK_BLOCKS] __attribute__((aligned(256)));

void *uf2_scratch(size_t *len)
{
    *len = sizeof(s_chunk);
    return s_chunk;
}
static uint8_t s_page[256] __attribute__((aligned(256)));

static bool fail(uf2_walk_t *w, const char *msg)
{
    snprintf(w->err, w->err_len, "%s", msg);
    return false;
}

// Read every block, check it, and pass the ones for this chip to w->on_block (if set).
// Fills w->info with what was found.
static bool walk(const char *path, uf2_walk_t *w)
{
    uf2_info_t *info = w->info;
    FRESULT fr = f_open(&w->f, sd_path(path), FA_READ);
    if (fr != FR_OK) {
        snprintf(w->err, w->err_len, "Cannot open the file (FatFS error %d).", fr);
        return false;
    }
    uint32_t size = (uint32_t)f_size(&w->f);
    if (w->on_block == NULL) {
        memset(info, 0, sizeof(*info));
        info->addr_lo = UINT32_MAX;
    }
    info->file_size = size;
    if (size < 512 || size % 512) {
        f_close(&w->f);
        return fail(w, "Not a UF2 file (the size is not a multiple of 512 bytes).");
    }

    uint32_t total = size / 512, family = 0, expected = 0, prev_no = 0, count = 0, other_family = 0;
    bool malformed = false, ram = false, ok = true;
    for (uint32_t idx = 0; ok && idx < total;) {
        UINT n = 0;
        uint32_t want = total - idx < CHUNK_BLOCKS ? total - idx : CHUNK_BLOCKS;
        if (f_read(&w->f, s_chunk, want * 512, &n) != FR_OK || n != want * 512) {
            ok = fail(w, "Read error on the SD card.");
            break;
        }
        for (uint32_t i = 0; ok && i < want; i++, idx++) {
            const struct uf2_block *b = &s_chunk[i];
            if (b->magic_start0 != UF2_MAGIC_START0 || b->magic_start1 != UF2_MAGIC_START1 || b->magic_end != UF2_MAGIC_END) {
                ok = fail(w, idx == 0 ? "Not a UF2 file." : "The UF2 file is damaged (bad block).");
                break;
            }
            if ((b->flags & UF2_FLAG_NOT_MAIN_FLASH) || !(b->flags & UF2_FLAG_FAMILY_ID_PRESENT)) {
                continue;
            }
            uint32_t fam = b->file_size;
            if (fam == ABSOLUTE_FAMILY_ID && b->target_addr == 0x10FFFF00 && b->block_no == 0) {
                malformed = b->num_blocks != 2; // RP2350-E10 workaround block
                continue;
            }
            if (!family_ok(fam) || (family && fam != family)) {
                other_family = other_family ? other_family : fam;
                continue;
            }
            if (b->payload_size != 256 || (b->target_addr & 0xFF) || b->num_blocks == 0 || b->block_no >= b->num_blocks) {
                ok = fail(w, "The UF2 file is damaged (bad block header).");
                break;
            }
            bool blk_ram = in_ram(b->target_addr);
            if (!blk_ram && (b->target_addr < XIP_BASE || b->target_addr + 256 > APP_END)) {
                char msg[96];
                snprintf(msg, sizeof(msg), "Address 0x%08lx is outside the program area (%lu KB).",
                         (unsigned long)b->target_addr, (unsigned long)uf2_flash_capacity() / 1024);
                ok = fail(w, msg);
                break;
            }
            if (count == 0) {
                family = fam;
                expected = b->num_blocks;
                ram = blk_ram;
                w->first_index = idx;
                if (b->block_no != (malformed ? 1u : 0u)) {
                    ok = fail(w, "The UF2 file is damaged (blocks out of order).");
                    break;
                }
            } else if (b->num_blocks != expected || b->block_no != prev_no + 1) {
                ok = fail(w, "The UF2 file is damaged (blocks out of order).");
                break;
            } else if (blk_ram != ram) {
                ok = fail(w, "The UF2 file mixes flash and RAM addresses.");
                break;
            }
            prev_no = b->block_no;
            count++;
            if (w->on_block) {
                ok = w->on_block(w, b);
                if (w->progress) {
                    w->progress(count, info->num_blocks);
                }
            } else {
                info->addr_lo = b->target_addr < info->addr_lo ? b->target_addr : info->addr_lo;
                info->addr_hi = b->target_addr + 256 > info->addr_hi ? b->target_addr + 256 : info->addr_hi;
            }
        }
    }
    f_close(&w->f);
    if (!ok) {
        return false;
    }
    if (count == 0) {
        char msg[96];
        if (other_family) {
            snprintf(msg, sizeof(msg), "This UF2 is for %s, not %s.", uf2_family_name(other_family), uf2_chip_name());
        } else {
            snprintf(msg, sizeof(msg), "The UF2 file has no blocks for %s.", uf2_chip_name());
        }
        return fail(w, msg);
    }
    if (count != expected - (malformed ? 1 : 0)) {
        return fail(w, "The UF2 file is incomplete (blocks missing).");
    }
    if (w->on_block == NULL) {
        info->family = family;
        info->num_blocks = count;
        info->ram_app = ram;
    }
    return true;
}

// ---- binary_info ------------------------------------------------------------------------------
typedef bool (*read_fn_t)(void *ctx, uint32_t addr, void *buf, uint32_t len);

// Read a string at a runtime address, mapping copied data (.data) back to where it is stored.
static void bi_string(read_fn_t rd, void *ctx, const uint32_t *map, int map_n, uint32_t addr, char *out, size_t len)
{
    for (int i = 0; i < map_n; i++) {
        uint32_t src = map[i * 3], dst = map[i * 3 + 1], dst_end = map[i * 3 + 2];
        if (addr >= dst && addr < dst_end) {
            addr = src + (addr - dst);
            break;
        }
    }
    out[0] = '\0';
    if (!rd(ctx, addr, out, len - 1)) {
        out[0] = '\0';
        return;
    }
    out[len - 1] = '\0';
    for (char *p = out; *p; p++) {
        if (*p < 0x20 || *p > 0x7E) {
            *p = '\0';
            break;
        }
    }
}

// Find the binary_info header near `start` and pull out the program name, version and date.
static void read_binary_info(read_fn_t rd, void *ctx, uint32_t start, uf2_info_t *info)
{
    uint32_t words[64], hdr[5] = {0};
    bool found = false;
    for (uint32_t base = start; !found && base < start + 0x500; base += sizeof(words)) {
        if (!rd(ctx, base, words, sizeof(words))) {
            return;
        }
        for (int i = 0; i < 64; i++) {
            if (words[i] == BI_MARKER_START) {
                found = rd(ctx, base + i * 4, hdr, sizeof(hdr)) && hdr[4] == BI_MARKER_END;
                if (found) {
                    break;
                }
            }
        }
    }
    if (!found || hdr[2] < hdr[1] || hdr[2] - hdr[1] > 4 * 256) {
        return;
    }
    uint32_t map[3 * 8];
    int map_n = 0;
    for (; map_n < 8; map_n++) {
        if (!rd(ctx, hdr[3] + map_n * 12, &map[map_n * 3], 12) || map[map_n * 3] == 0) {
            break;
        }
    }
    for (uint32_t p = hdr[1]; p < hdr[2]; p += 4) {
        uint32_t ptr, entry[3]; // {type | tag << 16, id, value}
        if (!rd(ctx, p, &ptr, 4) || !rd(ctx, ptr, entry, sizeof(entry))) {
            continue;
        }
        if ((entry[0] & 0xFFFF) != BI_TYPE_ID_AND_STRING || (entry[0] >> 16) != BI_TAG_RP) {
            continue;
        }
        if (entry[1] == BI_ID_PROGRAM_NAME) {
            bi_string(rd, ctx, map, map_n, entry[2], info->name, sizeof(info->name));
        } else if (entry[1] == BI_ID_PROGRAM_VERSION) {
            bi_string(rd, ctx, map, map_n, entry[2], info->version, sizeof(info->version));
        } else if (entry[1] == BI_ID_PROGRAM_DATE) {
            bi_string(rd, ctx, map, map_n, entry[2], info->build_date, sizeof(info->build_date));
        }
    }
}

// Reads by target address from a UF2 file (blocks are usually in address order).
typedef struct {
    FIL *f;
    uint32_t first_index, base, total;
    struct uf2_block blk;
    uint32_t cached; // target address of blk, 1 = none
} file_reader_t;

static bool file_block(file_reader_t *r, uint32_t page)
{
    if (r->cached == page) {
        return true;
    }
    UINT n;
    uint32_t guess = r->first_index + (page - r->base) / 256;
    if (page >= r->base && guess < r->total && f_lseek(r->f, (FSIZE_t)guess * 512) == FR_OK &&
        f_read(r->f, &r->blk, 512, &n) == FR_OK && n == 512 && r->blk.target_addr == page &&
        r->blk.magic_start0 == UF2_MAGIC_START0 && family_ok(r->blk.file_size)) {
        r->cached = page;
        return true;
    }
    for (uint32_t i = 0; i < r->total; i++) { // not where expected: search
        if (f_lseek(r->f, (FSIZE_t)i * 512) != FR_OK || f_read(r->f, &r->blk, 512, &n) != FR_OK || n != 512) {
            break;
        }
        if (r->blk.target_addr == page && r->blk.magic_start0 == UF2_MAGIC_START0 && family_ok(r->blk.file_size)) {
            r->cached = page;
            return true;
        }
    }
    r->cached = 1;
    return false;
}

static bool file_read(void *ctx, uint32_t addr, void *buf, uint32_t len)
{
    file_reader_t *r = ctx;
    uint8_t *out = buf;
    while (len) {
        uint32_t page = addr & ~0xFFu, off = addr & 0xFF, n = 256 - off < len ? 256 - off : len;
        if (!file_block(r, page)) {
            return false;
        }
        memcpy(out, &r->blk.data[off], n);
        out += n;
        addr += n;
        len -= n;
    }
    return true;
}

// Reads by address from the installed app, as the app sees its own addresses.
static bool flash_read(void *ctx, uint32_t addr, void *buf, uint32_t len)
{
    (void)ctx;
    if (addr < XIP_BASE || addr + len > APP_END || addr + len < addr) {
        return false;
    }
    memcpy(buf, (const void *)(addr + APP_OFFSET), len);
    return true;
}

// ---- inspect ----------------------------------------------------------------------------------
static uf2_walk_t s_walk;

bool uf2_inspect(const char *path, uf2_info_t *info, char *err, size_t err_len)
{
    uf2_walk_t *w = &s_walk;
    memset(w, 0, sizeof(*w));
    w->info = info;
    w->err = err;
    w->err_len = err_len;
    if (!walk(path, w)) {
        return false;
    }
    if (info->ram_app) {
        snprintf(err, err_len, "This is a RAM-only (no_flash) app; only apps built to run from flash can be installed.");
        return false;
    }
    if (info->addr_lo != XIP_BASE) {
        snprintf(err, err_len, "The app doesn't start at the beginning of flash (0x%08lx).", (unsigned long)info->addr_lo);
        return false;
    }

    // Program name etc. from binary_info (optional)
    FIL f;
    if (f_open(&f, sd_path(path), FA_READ) == FR_OK) {
        file_reader_t r = {.f = &f, .first_index = w->first_index, .base = info->addr_lo, .total = info->file_size / 512, .cached = 1};
        read_binary_info(file_read, &r, info->addr_lo, info);
        f_close(&f);
    }
    return true;
}

// ---- flashing ---------------------------------------------------------------------------------
#if PICO_RP2040
extern const uint8_t __boot2_start__[];
static uint8_t s_boot2[256] __attribute__((aligned(4)));
#endif

// Offsets are physical flash offsets. Nothing may run from flash meanwhile, so no interrupts.
static void erase_sector(uint32_t offset)
{
    uint32_t irq = save_and_disable_interrupts();
    flash_range_erase(offset, FLASH_SECTOR_SIZE);
    restore_interrupts(irq);
}

static bool program_page(uint32_t offset, const uint8_t *data)
{
    uint32_t irq = save_and_disable_interrupts();
    flash_range_program(offset, data, 256);
    restore_interrupts(irq);
    return memcmp((const void *)(XIP_BASE + offset), data, 256) == 0;
}

static bool flash_block(uf2_walk_t *w, const struct uf2_block *b)
{
    uint32_t addr = b->target_addr, sector = (addr - XIP_BASE) / FLASH_SECTOR_SIZE;
    uint32_t offset = addr - XIP_BASE + APP_OFFSET;
    if (sector >= sizeof(w->erased) * 8) {
        return fail(w, "Address beyond 16 MB.");
    }
#if !PICO_RP2040
    if (sector == 0) { // written last, by write_first_sector()
        w->first_pages++;
        w->written++;
        return true;
    }
#endif
    if (!(w->erased[sector / 8] & (1u << (sector % 8)))) {
        erase_sector(APP_OFFSET + sector * FLASH_SECTOR_SIZE);
#if PICO_RP2040
        // The chip boots through the boot2 in sector 0: always ours, which enters the loader.
        if (sector == 0 && !program_page(0, s_boot2)) {
            return fail(w, "Could not restore boot2.");
        }
#endif
        w->erased[sector / 8] |= 1u << (sector % 8);
    }
#if PICO_RP2040
    if (addr == XIP_BASE) {
        w->written++; // the app's own boot2 is replaced by ours
        return true;
    }
#endif
    memcpy(s_page, b->data, 256);
    if (!program_page(offset, s_page)) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Flash write failed at 0x%08lx.", (unsigned long)addr);
        return fail(w, msg);
    }
    w->written++;
    return true;
}

#if PICO_RP2040
static bool write_record(const uf2_info_t *info, const char *path)
{
    app_record_t r;
    memset(&r, 0, sizeof(r));
    r.magic = RECORD_MAGIC;
    r.size = info->addr_hi - XIP_BASE;
    r.crc = first_sector_crc(r.size);
    r.check = r.magic ^ r.size ^ r.crc ^ 0xffffffffu;
    const char *base = strrchr(path, '/');
    snprintf(r.file, sizeof(r.file), "%s", base ? base + 1 : path);
    memset(s_page, 0xFF, sizeof(s_page));
    memcpy(s_page, &r, sizeof(r));
    return program_page(RECORD_OFFSET, s_page);
}
#else
// The app's first sector holds its boot block (see app_present()), so it is programmed last, in a
// second pass over the file: the app only looks complete once everything else is in flash.
static bool write_first_sector(const char *path, uf2_walk_t *w, char *err, size_t err_len)
{
    uint32_t left = w->first_pages;
    if (f_open(&w->f, sd_path(path), FA_READ) != FR_OK || f_lseek(&w->f, (FSIZE_t)w->first_index * 512) != FR_OK) {
        f_close(&w->f);
        snprintf(err, err_len, "Cannot open the file again.");
        return false;
    }
    bool ok = true;
    for (uint32_t idx = w->first_index, total = w->info->file_size / 512; ok && left && idx < total;) {
        UINT n = 0;
        uint32_t want = total - idx < CHUNK_BLOCKS ? total - idx : CHUNK_BLOCKS;
        if (f_read(&w->f, s_chunk, want * 512, &n) != FR_OK || n != want * 512) {
            snprintf(err, err_len, "Read error on the SD card.");
            ok = false;
            break;
        }
        for (uint32_t i = 0; ok && left && i < want; i++, idx++) {
            const struct uf2_block *b = &s_chunk[i];
            if ((b->flags & UF2_FLAG_NOT_MAIN_FLASH) || !(b->flags & UF2_FLAG_FAMILY_ID_PRESENT) ||
                b->file_size != w->info->family || b->target_addr - XIP_BASE >= FLASH_SECTOR_SIZE) {
                continue; // the same blocks walk() took for sector 0
            }
            memcpy(s_page, b->data, 256);
            if (!program_page(APP_OFFSET + (b->target_addr - XIP_BASE), s_page)) {
                snprintf(err, err_len, "Flash write failed at 0x%08lx.", (unsigned long)b->target_addr);
                ok = false;
            }
            left--;
        }
    }
    f_close(&w->f);
    if (ok && left) {
        snprintf(err, err_len, "The first sector changed while flashing.");
        ok = false;
    }
    return ok;
}
#endif

#if !PICO_RP2040
// ---- resources in flash -----------------------------------------------------------------------
// The Wi-Fi firmware and the web page are linked into the last 256 KB of flash (.pml_res, see
// CMakeLists.txt) to leave RAM for the loader. pml2350.uf2 carries them as blocks of RES_FAMILY
// after its RAM blocks (add_res.py); the stub skips those, and uf2_res_install() copies them here.
#define RES_MAGIC  0x52574d50u // "PMWR"
#define RES_FAMILY 0x504d4c52u
#define LOADER_FILE "/pml2350.uf2"

typedef struct {
    uint32_t magic;
    uint32_t size;  // bytes after the header
    uint32_t crc;   // CRC-32 of those bytes
    uint32_t check; // magic ^ size ^ crc ^ 0xffffffff; 0xffffffff while being installed
} res_hdr_t;

_Static_assert(PML_RES_BASE == XIP_BASE + RES_OFFSET, "PML_RES_BASE must be the last 256 KB of flash");

// The start of .pml_res; add_res.py fills it in.
__attribute__((section(".pml_res.0hdr"), used)) const res_hdr_t s_res_hdr = {RES_MAGIC, 0, 0, 0};

// The header of the resources this build was linked with. add_res.py finds the marker in the RAM
// image and writes the header over it.
static const volatile res_hdr_t s_res_want __attribute__((aligned(16))) = {
    0x4d4c4d50u, 0x57534552u, 0x5f544e41u, 0x52444448u};

#define RES_FLASH ((const res_hdr_t *)(XIP_BASE + RES_OFFSET))

static bool res_hdr_valid(const res_hdr_t *h)
{
    return h->magic == RES_MAGIC && h->size <= RES_SIZE - sizeof(*h) &&
           h->check == (h->magic ^ h->size ^ h->crc ^ 0xffffffffu);
}

static bool res_payload_ok(const res_hdr_t *h)
{
    return crc32((const uint8_t *)(h + 1), h->size) == h->crc;
}

static int s_res_state = -1; // cached uf2_res_check(); -1 until the first check

res_state_t uf2_res_check(void)
{
    if (s_res_state >= 0) {
        return (res_state_t)s_res_state;
    }
    const res_hdr_t *f = RES_FLASH;
    res_state_t st;
    if (s_res_want.magic != RES_MAGIC) {
        st = RES_NO_FILE; // built without add_res.py
    } else if (f->magic == s_res_want.magic && f->size == s_res_want.size && f->crc == s_res_want.crc &&
               f->check == s_res_want.check && res_payload_ok(f)) {
        st = RES_OK;
    } else {
        const uint32_t *p = (const uint32_t *)f;
        unsigned i = 0;
        while (i < RES_SIZE / 4 && p[i] == 0xffffffffu) {
            i++;
        }
        if (i == RES_SIZE / 4) {
            st = RES_BLANK;
        } else if (f->magic == RES_MAGIC && ((res_hdr_valid(f) && res_payload_ok(f)) || f->check == 0xffffffffu)) {
            st = RES_OLD; // another loader version's, or an interrupted install
        } else {
            st = RES_USED; // an app keeps its data there
        }
    }
    s_res_state = st;
    return st;
}

bool uf2_res_ready(void)
{
    return uf2_res_check() == RES_OK;
}

bool uf2_res_install(uf2_progress_cb_t progress, char *err, size_t err_len)
{
    FIL f;
    if (s_res_want.magic != RES_MAGIC) {
        snprintf(err, err_len, "This loader was built without its resources (add_res.py).");
        return false;
    }
    if (f_open(&f, sd_path(LOADER_FILE), FA_READ) != FR_OK) {
        snprintf(err, err_len, "Cannot open " LOADER_FILE ".");
        return false;
    }
    s_res_state = -1;
    UINT n = 0;
    struct uf2_block *b = &s_chunk[0];
    uint32_t total_blocks = (uint32_t)(f_size(&f) / 512), idx = 0;
    // The loader's own blocks come first: skip them in one go
    if (f_read(&f, b, 512, &n) == FR_OK && n == 512 && b->magic_start0 == UF2_MAGIC_START0 &&
        b->num_blocks < total_blocks && f_lseek(&f, (FSIZE_t)b->num_blocks * 512) == FR_OK) {
        idx = b->num_blocks;
    } else {
        f_close(&f);
        snprintf(err, err_len, LOADER_FILE " is not a UF2 file.");
        return false;
    }
    uint64_t erased = 0; // one bit per 4 KB sector of the area
    uint32_t done = 0, want_pages = (sizeof(res_hdr_t) + s_res_want.size + 255) / 256;
    bool ok = true, got_hdr = false;
    for (; ok && idx < total_blocks;) {
        uint32_t chunk = total_blocks - idx < CHUNK_BLOCKS ? total_blocks - idx : CHUNK_BLOCKS;
        if (f_read(&f, s_chunk, chunk * 512, &n) != FR_OK || n != chunk * 512) {
            snprintf(err, err_len, "Read error on the SD card.");
            ok = false;
            break;
        }
        for (uint32_t i = 0; ok && i < chunk; i++, idx++) {
            b = &s_chunk[i];
            uint32_t off = b->target_addr - PML_RES_BASE;
            if (b->magic_start0 != UF2_MAGIC_START0 || b->magic_start1 != UF2_MAGIC_START1 ||
                b->magic_end != UF2_MAGIC_END || !(b->flags & UF2_FLAG_FAMILY_ID_PRESENT) ||
                b->file_size != RES_FAMILY) {
                continue;
            }
            if (b->payload_size != 256 || off >= RES_SIZE || (off & 0xFF)) {
                snprintf(err, err_len, "Bad resource block at 0x%08lx.", (unsigned long)b->target_addr);
                ok = false;
                break;
            }
            memcpy(s_page, b->data, 256);
            if (off == 0) { // the header: it must be the one this loader was linked with
                const res_hdr_t *h = (const res_hdr_t *)s_page;
                if (h->magic != s_res_want.magic || h->size != s_res_want.size || h->crc != s_res_want.crc ||
                    h->check != s_res_want.check) {
                    snprintf(err, err_len, LOADER_FILE " on the card is not the loader that is running.");
                    ok = false;
                    break;
                }
                ((res_hdr_t *)s_page)->check = 0xffffffffu; // written at the end
                got_hdr = true;
            }
            uint32_t sector = off / FLASH_SECTOR_SIZE;
            if (!(erased & (1ull << sector))) {
                erase_sector(RES_OFFSET + sector * FLASH_SECTOR_SIZE);
                erased |= 1ull << sector;
            }
            if (!program_page(RES_OFFSET + off, s_page)) {
                snprintf(err, err_len, "Flash write failed at 0x%08lx.", (unsigned long)b->target_addr);
                ok = false;
                break;
            }
            done++;
            if (progress) {
                progress(done, want_pages);
            }
        }
    }
    f_close(&f);
    if (ok && (!got_hdr || done != want_pages)) {
        snprintf(err, err_len, LOADER_FILE " has %lu of %lu resource blocks.", (unsigned long)done,
                 (unsigned long)want_pages);
        ok = false;
    }
    if (ok) { // complete: now the header's check (programming only clears bits, so this works)
        memcpy(s_page, (const void *)RES_FLASH, 256);
        ((res_hdr_t *)s_page)->check = s_res_want.check;
        if (!program_page(RES_OFFSET, s_page)) {
            snprintf(err, err_len, "Could not finish the resource header.");
            ok = false;
        }
    }
    s_res_state = -1;
    if (ok && uf2_res_check() != RES_OK) {
        snprintf(err, err_len, "The resources in flash failed their check.");
        ok = false;
    }
    return ok;
}
#endif

bool uf2_flash(const char *path, const uf2_info_t *info, uf2_progress_cb_t progress, char *err, size_t err_len)
{
    if (info->ram_app) {
        snprintf(err, err_len, "RAM-only apps are not supported.");
        return false;
    }
#if PICO_RP2040
    memcpy(s_boot2, __boot2_start__, sizeof(s_boot2));
    erase_sector(RECORD_OFFSET); // until the new app is complete there is none
#else
    erase_sector(APP_OFFSET); // until the new app is complete there is none
#endif
    uf2_walk_t *w = &s_walk;
    memset(w, 0, sizeof(*w));
    uf2_info_t copy = *info;
    w->info = &copy;
    w->on_block = flash_block;
    w->progress = progress;
    w->err = err;
    w->err_len = err_len;
    if (!walk(path, w)) {
        return false;
    }
    if (w->written == 0) {
        snprintf(err, err_len, "Nothing was written: wrong platform.");
        return false;
    }
    if (w->written != info->num_blocks) {
        snprintf(err, err_len, "Only %lu of %lu blocks were written.", (unsigned long)w->written, (unsigned long)info->num_blocks);
        return false;
    }
#if PICO_RP2040
    if (!write_record(info, path)) {
        snprintf(err, err_len, "Could not write the app record.");
        return false;
    }
#else
    if (!write_first_sector(path, w, err, err_len)) {
        return false;
    }
    const char *base = strrchr(path, '/');
    config_set(APP_CONFIG_KEY, base ? base + 1 : path);
#endif
    return true;
}

// ---- booting ----------------------------------------------------------------------------------
void uf2_boot_installed(void)
{
    sdcard_unmount();
#if PICO_RP2040
    reboot_into_app();
#else
    // Ask the boot stub for the app, the uf2loader way
    printf("boot: starting the app\n");
    stdio_flush();
    watchdog_hw->scratch[1] = BL_MODE_DEFAULT;
    watchdog_hw->scratch[0] = BL_MAGIC;
    watchdog_reboot(0, 0, 0);
    for (;;) {
        tight_loop_contents();
    }
#endif
}

bool uf2_installed(char *name, size_t name_len, char *version, size_t version_len)
{
    uf2_info_t info;
    memset(&info, 0, sizeof(info));
#if PICO_RP2040
    const app_record_t *r = installed_record();
    if (!r) {
        return false;
    }
    read_binary_info(flash_read, NULL, XIP_BASE + 0x100, &info);
    const char *file = r->file;
#else
    if (!app_present()) {
        return false;
    }
    read_binary_info(flash_read, NULL, XIP_BASE, &info);
    char file[64];
    if (!config_get(APP_CONFIG_KEY, file, sizeof(file))) {
        snprintf(file, sizeof(file), "(unknown)");
    }
#endif
    if (info.name[0]) {
        snprintf(name, name_len, "%s", info.name);
    } else {
        snprintf(name, name_len, "%.63s", file);
    }
    snprintf(version, version_len, "%s", info.version);
    return true;
}
