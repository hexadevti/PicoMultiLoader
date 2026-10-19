// sdcard.c - SD card over SPI (SDv1, SDv2 and SDHC/SDXC), plus the FatFS disk glue.
#include "sdcard.h"

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "board.h"
#include "diskio.h"

#define INIT_HZ        (400 * 1000)
#define READY_TIMEOUT  500000  // us: card busy after a write
#define TOKEN_TIMEOUT  250000  // us: wait for a data token
#define INIT_TIMEOUT   1000000 // us: ACMD41 loop

#define CMD0   0  // GO_IDLE_STATE
#define CMD8   8  // SEND_IF_COND
#define CMD9   9  // SEND_CSD
#define CMD12  12 // STOP_TRANSMISSION
#define CMD16  16 // SET_BLOCKLEN
#define CMD17  17 // READ_SINGLE_BLOCK
#define CMD18  18 // READ_MULTIPLE_BLOCK
#define CMD24  24 // WRITE_BLOCK
#define CMD25  25 // WRITE_MULTIPLE_BLOCK
#define CMD55  55 // APP_CMD
#define CMD58  58 // READ_OCR
#define ACMD41 (0x80 | 41) // SD_SEND_OP_COND

static bool s_bus_ready;
static bool s_card_ok;   // initialised and answering
static bool s_block_addr; // SDHC/SDXC: addresses are sector numbers
static uint32_t s_sectors;
static bool s_mounted;
static FATFS s_fs;

// ---- SPI --------------------------------------------------------------------------------------
static inline uint8_t xfer(uint8_t b)
{
    uint8_t r;
    spi_write_read_blocking(SD_SPI, &b, &r, 1);
    return r;
}

static void deselect(void)
{
    gpio_put(SD_CS_PIN, 1);
    xfer(0xFF); // let the card release MISO
}

static bool wait_ready(uint32_t timeout_us)
{
    absolute_time_t until = make_timeout_time_us(timeout_us);
    do {
        if (xfer(0xFF) == 0xFF) {
            return true;
        }
    } while (!time_reached(until));
    return false;
}

static bool select_card(void)
{
    gpio_put(SD_CS_PIN, 0);
    xfer(0xFF);
    if (wait_ready(READY_TIMEOUT)) {
        return true;
    }
    deselect();
    return false;
}

// Send a command (CS stays low afterwards). Returns R1, or 0xFF on a timeout.
static uint8_t send_cmd(uint8_t cmd, uint32_t arg)
{
    if (cmd & 0x80) { // ACMD<n> = CMD55 + CMD<n>
        cmd &= 0x7F;
        uint8_t r = send_cmd(CMD55, 0);
        if (r > 1) {
            return r;
        }
    }
    if (cmd != CMD12) {
        deselect();
        if (!select_card()) {
            return 0xFF;
        }
    }
    uint8_t frame[6] = {
        (uint8_t)(0x40 | cmd), (uint8_t)(arg >> 24), (uint8_t)(arg >> 16), (uint8_t)(arg >> 8), (uint8_t)arg,
        cmd == CMD0 ? 0x95 : cmd == CMD8 ? 0x87 : 0x01, // CRC only matters before CRC is switched off
    };
    spi_write_blocking(SD_SPI, frame, sizeof(frame));
    if (cmd == CMD12) {
        xfer(0xFF); // stuff byte
    }
    uint8_t r;
    for (int i = 0; i < 10; i++) {
        r = xfer(0xFF);
        if (!(r & 0x80)) {
            break;
        }
    }
    return r;
}

static bool read_block(uint8_t *buf, size_t len)
{
    absolute_time_t until = make_timeout_time_us(TOKEN_TIMEOUT);
    uint8_t token;
    do {
        token = xfer(0xFF);
    } while (token == 0xFF && !time_reached(until));
    if (token != 0xFE) {
        return false;
    }
    spi_read_blocking(SD_SPI, 0xFF, buf, len);
    xfer(0xFF); // CRC
    xfer(0xFF);
    return true;
}

static bool write_block(const uint8_t *buf, uint8_t token)
{
    if (!wait_ready(READY_TIMEOUT)) {
        return false;
    }
    xfer(token);
    if (token == 0xFD) { // stop transmission
        return true;
    }
    spi_write_blocking(SD_SPI, buf, 512);
    xfer(0xFF); // CRC
    xfer(0xFF);
    return (xfer(0xFF) & 0x1F) == 0x05; // data accepted
}

// ---- card -------------------------------------------------------------------------------------
static void bus_init(void)
{
    if (s_bus_ready) {
        return;
    }
    gpio_init(SD_DET_PIN);
    gpio_set_dir(SD_DET_PIN, GPIO_IN);
    gpio_pull_up(SD_DET_PIN);
    gpio_init(SD_CS_PIN);
    gpio_set_dir(SD_CS_PIN, GPIO_OUT);
    gpio_put(SD_CS_PIN, 1);
    spi_init(SD_SPI, INIT_HZ);
    spi_set_format(SD_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(SD_SCK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(SD_MOSI_PIN, GPIO_FUNC_SPI);
    gpio_set_function(SD_MISO_PIN, GPIO_FUNC_SPI);
    gpio_pull_up(SD_MISO_PIN);
    s_bus_ready = true;
}

bool sdcard_present(void)
{
    bus_init();
    return !gpio_get(SD_DET_PIN);
}

static uint32_t read_capacity(void)
{
    uint8_t csd[16];
    if (send_cmd(CMD9, 0) != 0 || !read_block(csd, sizeof(csd))) {
        return 0;
    }
    if ((csd[0] >> 6) == 1) { // CSD v2: (C_SIZE + 1) * 512 KB
        uint32_t c_size = ((uint32_t)(csd[7] & 0x3F) << 16) | ((uint32_t)csd[8] << 8) | csd[9];
        return (c_size + 1) * 1024;
    }
    uint32_t read_bl_len = csd[5] & 15;
    uint32_t c_size = ((uint32_t)(csd[6] & 3) << 10) | ((uint32_t)csd[7] << 2) | (csd[8] >> 6);
    uint32_t mult = ((csd[9] & 3) << 1) | (csd[10] >> 7);
    return (c_size + 1) << (mult + 2 + read_bl_len - 9);
}

static bool card_init(void)
{
    bus_init();
    s_card_ok = false;
    if (!sdcard_present()) {
        return false;
    }
    spi_set_baudrate(SD_SPI, INIT_HZ);
    gpio_put(SD_CS_PIN, 1);
    for (int i = 0; i < 10; i++) { // 80+ clocks with CS high
        xfer(0xFF);
    }

    uint8_t r = 0xFF;
    for (int i = 0; i < 10 && r != 1; i++) {
        r = send_cmd(CMD0, 0);
    }
    if (r != 1) {
        deselect();
        return false;
    }

    bool v2 = false;
    uint8_t ocr[4];
    if (send_cmd(CMD8, 0x1AA) == 1) {
        spi_read_blocking(SD_SPI, 0xFF, ocr, 4);
        if (ocr[2] != 0x01 || ocr[3] != 0xAA) {
            deselect();
            return false; // can't run at 2.7-3.6 V
        }
        v2 = true;
    }

    absolute_time_t until = make_timeout_time_us(INIT_TIMEOUT);
    do {
        r = send_cmd(ACMD41, v2 ? 1u << 30 : 0);
    } while (r == 1 && !time_reached(until));
    if (r != 0) {
        deselect();
        return false;
    }

    s_block_addr = false;
    if (v2 && send_cmd(CMD58, 0) == 0) {
        spi_read_blocking(SD_SPI, 0xFF, ocr, 4);
        s_block_addr = (ocr[0] & 0x40) != 0; // CCS
    }
    if (!s_block_addr && send_cmd(CMD16, 512) != 0) {
        deselect();
        return false;
    }
    deselect();

    spi_set_baudrate(SD_SPI, SD_SPI_HZ);
    s_sectors = read_capacity();
    deselect();
    s_card_ok = s_sectors != 0;
    return s_card_ok;
}

bool sd_read_sectors(uint32_t lba, uint8_t *buf, uint32_t count)
{
    if (!s_card_ok || !count) {
        return false;
    }
    uint32_t addr = s_block_addr ? lba : lba * 512;
    bool ok;
    if (count == 1) {
        ok = send_cmd(CMD17, addr) == 0 && read_block(buf, 512);
    } else {
        ok = send_cmd(CMD18, addr) == 0;
        for (; ok && count; count--, buf += 512) {
            ok = read_block(buf, 512);
        }
        send_cmd(CMD12, 0);
    }
    deselect();
    return ok;
}

bool sd_write_sectors(uint32_t lba, const uint8_t *buf, uint32_t count)
{
    if (!s_card_ok || !count) {
        return false;
    }
    uint32_t addr = s_block_addr ? lba : lba * 512;
    bool ok;
    if (count == 1) {
        ok = send_cmd(CMD24, addr) == 0 && write_block(buf, 0xFE);
    } else {
        ok = send_cmd(CMD25, addr) == 0;
        for (; ok && count; count--, buf += 512) {
            ok = write_block(buf, 0xFC);
        }
        if (!write_block(NULL, 0xFD)) {
            ok = false;
        }
    }
    if (!wait_ready(READY_TIMEOUT)) { // finish programming before releasing the card
        ok = false;
    }
    deselect();
    return ok;
}

// ---- filesystem -------------------------------------------------------------------------------
bool sdcard_mount(void)
{
    if (s_mounted) {
        return true;
    }
    if (!card_init()) {
        printf("sd: no card\n");
        return false;
    }
    FRESULT fr = f_mount(&s_fs, "", 1);
    if (fr != FR_OK) {
        printf("sd: mount failed (%d)\n", fr);
        return false;
    }
    s_mounted = true;
    printf("sd: mounted, %lu MB\n", (unsigned long)(s_sectors / 2048));
    return true;
}

void sdcard_unmount(void)
{
    if (s_mounted) {
        f_unmount("");
        s_mounted = false;
    }
}

bool sdcard_space(uint64_t *total, uint64_t *free_bytes)
{
    FATFS *fs;
    DWORD free_clusters;
    if (!s_mounted || f_getfree("", &free_clusters, &fs) != FR_OK) {
        return false;
    }
    uint64_t cluster = (uint64_t)fs->csize * 512;
    *total = (uint64_t)(fs->n_fatent - 2) * cluster;
    *free_bytes = (uint64_t)free_clusters * cluster;
    return true;
}

bool sdcard_open_raw(uint32_t *sector_count)
{
    sdcard_unmount();
    if (!card_init()) {
        return false;
    }
    *sector_count = s_sectors;
    return true;
}

void sdcard_close_raw(void)
{
}

// ---- FatFS disk interface ---------------------------------------------------------------------
DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv != 0) {
        return STA_NOINIT;
    }
    return (s_card_ok ? 0 : STA_NOINIT) | (sdcard_present() ? 0 : STA_NODISK);
}

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv == 0 && !s_card_ok) {
        card_init();
    }
    return disk_status(pdrv);
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0 || !s_card_ok) {
        return RES_NOTRDY;
    }
    return sd_read_sectors(sector, buff, count) ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0 || !s_card_ok) {
        return RES_NOTRDY;
    }
    return sd_write_sectors(sector, buff, count) ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv != 0 || !s_card_ok) {
        return RES_NOTRDY;
    }
    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK; // writes finish before sd_write_sectors() returns
    case GET_SECTOR_COUNT:
        *(LBA_t *)buff = s_sectors;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = 512;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}
