// uf2_image.c - UF2 checks, flashing and launching for the PicoCalc UF2 Loader (stage3).
//
// The stage3 conventions used here (watchdog scratch command, bootloader info in the vector table
// hole, RP2350 app partition mapping) follow pelrun's uf2loader; no code is taken from it.
#include "uf2_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/flash.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include "boot/uf2.h"
#include "ff.h"
#include "sdcard.h"
#if PICO_RP2350
#include "hardware/structs/qmi.h"
#include "boot/picobin.h"
#endif

// ---- stage3 interface -------------------------------------------------------------------------
#define BL_MAGIC 0xe98cc638u // watchdog scratch[0] command and bootloader info magic

enum { BOOT_DEFAULT, BOOT_SD, BOOT_UPDATE, BOOT_RAM }; // watchdog scratch[1]

typedef struct {
    uint32_t magic;
    uint32_t flash_end;
#if PICO_RP2040
    char filename[20]; // not necessarily NUL-terminated
#endif
} bl_info_t;

// The bootloader info sits in reserved vector table entries: stage3 fills it in our RAM image
// (blinfo) and expects it in the app's image in flash (proginfo).
#if PICO_RP2040
#define VECTOR_HOLE 0x110 // the vector table follows the 256-byte boot2
#else
#define VECTOR_HOLE 0x20
#endif
#define PROGINFO_ADDR (XIP_BASE + VECTOR_HOLE)
#define BLINFO_ADDR   (SRAM_BASE + VECTOR_HOLE)

#define BI_TAG_RP              0x5052 // 'R','P'
#define BI_TYPE_ID_AND_STRING  6
#define BI_ID_PROGRAM_NAME     0x02031c86
#define BI_ID_PROGRAM_VERSION  0x11a9bc3a
#define BI_ID_PROGRAM_DATE     0x9da22254
#define BI_MARKER_START        0x7188ebf2
#define BI_MARKER_END          0xe71aa390

static bool s_bl_ok;
static uint32_t s_prog_end; // end of the program area (XIP address)
static char s_ram_path[128]; // must stay put across the reboot: stage3 reads it from our RAM

// ---- platform ---------------------------------------------------------------------------------
#if PICO_RP2350
// Map the app partition (partition 0) at XIP_BASE, as stage3 does for apps, so UF2 addresses and
// runtime flash operations line up with it.
static void map_app_partition(void)
{
    uint8_t *work = malloc(4096);
    if (!work) {
        return;
    }
    int rc = rom_load_partition_table(work, 4096, false);
    free(work);
    if (rc != 0) {
        printf("uf2: no partition table (%d)\n", rc);
        return;
    }
    uint32_t pt[3];
    rc = rom_get_partition_table_info(pt, 3, PT_INFO_PARTITION_LOCATION_AND_FLAGS | PT_INFO_SINGLE_PARTITION | (0u << 24));
    if (rc != 3 || !(pt[0] & PT_INFO_PARTITION_LOCATION_AND_FLAGS)) {
        printf("uf2: no app partition (%d)\n", rc);
        return;
    }
    uint32_t first = (pt[1] & PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS) >> PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB;
    uint32_t last = (pt[1] & PICOBIN_PARTITION_LOCATION_LAST_SECTOR_BITS) >> PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB;
    uint32_t left = last >= first ? last - first + 1 : 0; // 4 KB sectors
    for (int i = 0; i < 4; i++) { // each window maps up to 4 MB (0x400 sectors)
        uint32_t size = left > 0x400 ? 0x400 : left;
        qmi_hw->atrans[i] = (size << QMI_ATRANS0_SIZE_LSB) | ((i * 0x400u + first) & QMI_ATRANS0_BASE_BITS);
        left -= size;
    }
    rom_flash_flush_cache();
    printf("uf2: app partition at 0x%lx, %lu KB\n", (unsigned long)first * 4096, (unsigned long)(last - first + 1) * 4);
}
#endif

void uf2_platform_init(void)
{
#if PICO_RP2350
    map_app_partition();
#endif
    bl_info_t bl;
    memcpy(&bl, (const void *)BLINFO_ADDR, sizeof(bl));
    s_bl_ok = bl.magic == BL_MAGIC && bl.flash_end > XIP_BASE && bl.flash_end <= XIP_BASE + 16 * 1024 * 1024;
    s_prog_end = s_bl_ok ? bl.flash_end : XIP_BASE;
    printf("uf2: bootloader info %s, program area %lu KB\n", s_bl_ok ? "ok" : "MISSING",
           (unsigned long)(s_prog_end - XIP_BASE) / 1024);
}

bool uf2_bootloader_ok(void)
{
    return s_bl_ok;
}

uint32_t uf2_flash_capacity(void)
{
    return s_prog_end - XIP_BASE;
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

// ---- walking a UF2 file -----------------------------------------------------------------------
#define CHUNK_BLOCKS 8

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
};

static struct uf2_block s_chunk[CHUNK_BLOCKS] __attribute__((aligned(256)));
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
            if (!blk_ram && (b->target_addr < XIP_BASE || b->target_addr + 256 > s_prog_end)) {
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

static bool flash_read(void *ctx, uint32_t addr, void *buf, uint32_t len)
{
    (void)ctx;
    if (addr < XIP_BASE || addr + len > s_prog_end || addr + len < addr) {
        return false;
    }
    memcpy(buf, (const void *)addr, len);
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
    if (!s_bl_ok) {
        snprintf(err, err_len, "Invalid bootloader: start PicoMultiLoader from the PicoCalc UF2 Loader "
                 "(stage3), as BOOT%s.UF2 on the SD card.", uf2_chip_name() + 2);
        return false;
    }
    if (!walk(path, w)) {
        return false;
    }
    if (!info->ram_app && info->addr_lo != XIP_BASE) {
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
#if PICO_RP2350
// Runtime addresses: the bootrom translates them through the app partition mapping (QMI ATRANS).
#define CFLASH_FLAGS(op) ((cflash_flags_t){ .flags = ((op) << CFLASH_OP_LSB) |                                                                             (CFLASH_SECLEVEL_VALUE_SECURE << CFLASH_SECLEVEL_LSB) |                                                      (CFLASH_ASPACE_VALUE_RUNTIME << CFLASH_ASPACE_LSB) })
#endif

static bool erase_sector(uint32_t addr)
{
#if PICO_RP2040
    flash_range_erase(addr - XIP_BASE, FLASH_SECTOR_SIZE);
    return true;
#else
    int rc = rom_flash_op(CFLASH_FLAGS(CFLASH_OP_VALUE_ERASE), addr, FLASH_SECTOR_SIZE, NULL);
    rom_flash_flush_cache();
    return rc == 0;
#endif
}

static bool program_page(uint32_t addr, const uint8_t *data)
{
#if PICO_RP2040
    flash_range_program(addr - XIP_BASE, data, 256);
#else
    if (rom_flash_op(CFLASH_FLAGS(CFLASH_OP_VALUE_PROGRAM), addr, 256, (uint8_t *)data) != 0) {
        return false;
    }
    rom_flash_flush_cache();
#endif
    return memcmp((const void *)addr, data, 256) == 0;
}

static bool flash_block(uf2_walk_t *w, const struct uf2_block *b)
{
    uint32_t addr = b->target_addr, sector = (addr - XIP_BASE) / FLASH_SECTOR_SIZE;
    if (sector >= sizeof(w->erased) * 8) {
        return fail(w, "Address beyond 16 MB.");
    }
    if (!(w->erased[sector / 8] & (1u << (sector % 8)))) {
#if PICO_RP2040
        // Sector 0 holds the boot2 stage3 relies on: keep it, whatever the app brings.
        static uint8_t boot2[256] __attribute__((aligned(4)));
        if (sector == 0) {
            memcpy(boot2, (const void *)XIP_BASE, sizeof(boot2));
        }
#endif
        if (!erase_sector(XIP_BASE + sector * FLASH_SECTOR_SIZE)) {
            return fail(w, "Flash erase failed.");
        }
#if PICO_RP2040
        if (sector == 0 && !program_page(XIP_BASE, boot2)) {
            return fail(w, "Could not restore boot2.");
        }
#endif
        w->erased[sector / 8] |= 1u << (sector % 8);
    }
#if PICO_RP2040
    if (addr == XIP_BASE) {
        w->written++; // the app's own boot2 is replaced by the one already there
        return true;
    }
#endif
    memcpy(s_page, b->data, 256);
    uint32_t pi = PROGINFO_ADDR;
    if (pi >= addr && pi < addr + 256) { // the loader fills this in at the end
        memset(&s_page[pi - addr], 0xFF, sizeof(bl_info_t));
    }
    if (!program_page(addr, s_page)) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Flash write failed at 0x%08lx.", (unsigned long)addr);
        return fail(w, msg);
    }
    w->written++;
    return true;
}

static bool write_proginfo(const uf2_info_t *info, const char *path)
{
    uint32_t page = PROGINFO_ADDR & ~0xFFu;
    memcpy(s_page, (const void *)page, 256);
    bl_info_t bl;
    memset(&bl, 0, sizeof(bl));
    bl.magic = BL_MAGIC;
    bl.flash_end = info->addr_hi;
#if PICO_RP2040
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t n = strlen(base); // not NUL-terminated when it fills the field
    memcpy(bl.filename, base, n < sizeof(bl.filename) ? n : sizeof(bl.filename));
#else
    (void)path;
#endif
    memcpy(&s_page[PROGINFO_ADDR - page], &bl, sizeof(bl));
    return program_page(page, s_page); // those bytes were left erased
}

bool uf2_flash(const char *path, const uf2_info_t *info, uf2_progress_cb_t progress, char *err, size_t err_len)
{
    if (info->ram_app) {
        snprintf(err, err_len, "This is a RAM app: it runs without flashing.");
        return false;
    }
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
    if (!write_proginfo(info, path)) {
        snprintf(err, err_len, "Could not write the program info.");
        return false;
    }
    return true;
}

// ---- booting ----------------------------------------------------------------------------------
static void __attribute__((noreturn)) reboot_with(uint32_t mode, uint32_t arg)
{
    sdcard_unmount();
    if (mode == BOOT_DEFAULT) {
        watchdog_hw->scratch[0] = 0;
    } else {
        watchdog_hw->scratch[1] = mode;
        watchdog_hw->scratch[2] = arg;
        watchdog_hw->scratch[0] = BL_MAGIC;
    }
    watchdog_reboot(0, 0, 0);
    for (;;) {
        tight_loop_contents();
    }
}

void uf2_boot_installed(void)
{
    reboot_with(BOOT_DEFAULT, 0);
}

// Stage3 opens the file by its 8.3 name: build that from each part of the long path.
bool uf2_run_ram_app(const char *path, char *err, size_t err_len)
{
    char long_path[256];
    size_t out = 0;
    long_path[0] = '\0';
    s_ram_path[0] = '\0';
    for (const char *p = path; *p;) {
        while (*p == '/') {
            p++;
        }
        const char *end = strchr(p, '/');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (!n) {
            break;
        }
        size_t ll = strlen(long_path);
        if (ll + 1 + n >= sizeof(long_path)) {
            snprintf(err, err_len, "The path is too long.");
            return false;
        }
        long_path[ll] = '/';
        memcpy(long_path + ll + 1, p, n);
        long_path[ll + 1 + n] = '\0';
        FILINFO fno;
        if (f_stat(long_path, &fno) != FR_OK) {
            snprintf(err, err_len, "Cannot find %s.", long_path);
            return false;
        }
        const char *sfn = fno.altname[0] ? fno.altname : fno.fname;
        int w = snprintf(s_ram_path + out, sizeof(s_ram_path) - out, "/%s", sfn);
        if (w < 0 || out + w >= sizeof(s_ram_path)) {
            snprintf(err, err_len, "The path is too long.");
            return false;
        }
        out += w;
        p += n;
    }
    printf("uf2: run RAM app %s\n", s_ram_path);
    reboot_with(BOOT_RAM, (uint32_t)s_ram_path);
}

bool uf2_installed(char *name, size_t name_len, char *version, size_t version_len)
{
    bl_info_t bl;
    memcpy(&bl, (const void *)PROGINFO_ADDR, sizeof(bl));
    if (!s_bl_ok || bl.magic != BL_MAGIC) {
        return false;
    }
    uf2_info_t info;
    memset(&info, 0, sizeof(info));
#if PICO_RP2040
    read_binary_info(flash_read, NULL, XIP_BASE + 0x100, &info);
#else
    read_binary_info(flash_read, NULL, XIP_BASE, &info);
#endif
    if (info.name[0]) {
        snprintf(name, name_len, "%s", info.name);
    } else {
#if PICO_RP2040
        snprintf(name, name_len, "%.20s", bl.filename);
#else
        snprintf(name, name_len, "(unnamed app)");
#endif
    }
    snprintf(version, version_len, "%s", info.version);
    return true;
}
