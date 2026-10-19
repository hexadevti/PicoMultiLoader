// boot2 for the RP2040 build: the SDK's W25Q080 second stage, but when the bootrom runs it, it
// enters PicoMultiLoader (vector table at 0x101C0000) instead of the start of flash. Called as a
// function (the SDK's flash_range_* do that to restore XIP), it still just returns.
//
// exit_from_boot2.S vectors through (XIP_BASE + 0x100); nothing else in boot2 uses XIP_BASE.
#include "hardware/regs/addressmap.h"
#undef XIP_BASE
#define XIP_BASE (PML_LOADER_BASE - 0x100)

#include "boot2_w25q080.S"
