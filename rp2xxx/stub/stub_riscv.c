// stub_riscv.c - the boot stub's RISC-V half (RP2350), for apps built for the Hazard3 cores.
//
// The ARM stub can't start a RISC-V app itself: when chain_image meets an IMAGE_DEF for the other
// CPU, the bootrom reboots into that architecture and boots flash again from the start. This half
// sits in the same block loop as the ARM stub, so that RISC-V boot lands here (the bootrom picks
// the IMAGE_DEF for the CPU it boots on) and chains the app again, now on the right CPU.
//
//   - the uf2loader menu request (watchdog scratch, mode != 0): back to ARM, the ARM stub shows it;
//   - otherwise: chain the app. If that returns there is no app to run here: back to ARM asking for
//     the menu (not plain ARM, which would chain the app and land here again, for ever).
//
// Bare metal, no SDK runtime: it must fit in the stub's 8 KB beside the ARM stub. The bootrom
// enters it at twin_main with sp set (the ENTRY_POINT item), executing in place from flash.
// stub/add_riscv_twin.py links it into the ARM stub's block loop.
#include <stdint.h>

#ifndef FLASH_SIZE
#define FLASH_SIZE (4u * 1024 * 1024)
#endif
#define XIP_BASE   0x10000000u
#define APP_OFFSET 0x2000u
#define APP_SIZE   (FLASH_SIZE - APP_OFFSET)
#define WORKAREA   0x20000000u // chain_image's work area: main RAM is free here
#define ROM_STACK  0x20001000u // and the bootrom's (see twin_main)
#define ROM_STACK_SIZE 0x3000u
#define SCRATCH    ((volatile uint32_t *)0x400d800cu) // watchdog scratch[0..]

#define BL_MAGIC        0xe98cc638u // uf2loader: scratch[0]; scratch[1] = mode
#define BL_MODE_DEFAULT 0           // start the app
#define BL_MODE_MENU    1

// bootrom (see pico/bootrom.h, boot/bootrom_constants.h)
#define ROM_TABLE_LOOKUP_ENTRY 0x7dfau // BOOTROM_TABLE_LOOKUP_ENTRY_OFFSET on RISC-V
#define RT_FLAG_FUNC_RISCV     0x0001u
#define ROM_CODE(a, b)         ((uint32_t)(a) | (uint32_t)(b) << 8)
#define REBOOT2_FLAG_REBOOT_TO_ARM         0x10u
#define REBOOT2_FLAG_NO_RETURN_ON_SUCCESS 0x100u

typedef void *(*rom_lookup_fn)(uint32_t code, uint32_t mask);
typedef int (*rom_chain_image_fn)(uint8_t *workarea, uint32_t workarea_size, uint32_t base, uint32_t size);
typedef int (*rom_set_bootrom_stack_fn)(uint32_t base_size[2]);
typedef int (*rom_reboot_fn)(uint32_t flags, uint32_t delay_ms, uint32_t p0, uint32_t p1);

static void *rom_func(uint32_t code)
{
    rom_lookup_fn lookup = (rom_lookup_fn)(uintptr_t)*(volatile uint16_t *)ROM_TABLE_LOOKUP_ENTRY;
    return lookup(code, RT_FLAG_FUNC_RISCV);
}

void __attribute__((noreturn, section(".text.entry"))) twin_main(void)
{
    if (SCRATCH[0] != BL_MAGIC || SCRATCH[1] == BL_MODE_DEFAULT) {
        SCRATCH[0] = 0;
        // Most bootrom functions are Arm code, emulated on RISC-V on a small stack in boot RAM:
        // too small for chain_image, which scans flash like a boot does. Give it main RAM.
        uint32_t stack[2] = {ROM_STACK, ROM_STACK_SIZE};
        ((rom_set_bootrom_stack_fn)rom_func(ROM_CODE('S', 'S')))(stack);
        rom_chain_image_fn chain = (rom_chain_image_fn)rom_func(ROM_CODE('C', 'I'));
        chain((uint8_t *)WORKAREA, 4096, XIP_BASE + APP_OFFSET, APP_SIZE);
        SCRATCH[1] = BL_MODE_MENU;
        SCRATCH[0] = BL_MAGIC;
    }
    rom_reboot_fn reboot = (rom_reboot_fn)rom_func(ROM_CODE('R', 'B'));
    reboot(REBOOT2_FLAG_REBOOT_TO_ARM | REBOOT2_FLAG_NO_RETURN_ON_SUCCESS, 10, 0, 0);
    for (;;) {
        __asm volatile("wfi");
    }
}
