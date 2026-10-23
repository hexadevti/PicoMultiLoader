/* stub_riscv.ld - the boot stub's RISC-V half: code only, at TWIN_BASE (-Wl,--defsym), run in place. */
SECTIONS {
    . = TWIN_BASE;
    .text : { KEEP(*(.text.entry)) *(.text*) *(.rodata*) *(.srodata*) }
    .data : { *(.data*) *(.sdata*) *(.bss*) *(.sbss*) *(COMMON) }
    ASSERT(SIZEOF(.data) == 0, "stub_riscv.c must not use RAM variables")
}
