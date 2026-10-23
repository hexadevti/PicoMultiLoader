#!/usr/bin/env python3
# add_riscv_twin.py - put the boot stub's RISC-V half (stub_riscv.c) into the ARM stub's image.
#
#   add_riscv_twin.py <arm.bin> <twin.bin> <twin_offset> <out.bin> <out.uf2>
#
# The twin's code goes at <twin_offset> in flash (it is linked there and runs in place), followed
# by a block with its IMAGE_DEF (RISC-V EXE, entry point = its first byte). That block is linked
# into the ARM stub's block loop, making a "universal" image: the bootrom boots the IMAGE_DEF for
# the CPU it is running on. The ARM image is otherwise unchanged.
import struct
import sys

XIP_BASE = 0x10000000
STUB_SIZE = 0x2000            # the app partition starts here
TWIN_SP = 0x20082000          # SRAM_END
MARKER_START = 0xffffded3
MARKER_END = 0xab123579
ITEM_IMAGE_TYPE = 0x42
ITEM_ENTRY_POINT = 0x44
ITEM_HASH_DEF = 0x47
ITEM_LAST = 0xff
IMAGE_TYPE_RISCV_EXE = 0x1 | (1 << 8) | (1 << 12)   # EXE, CPU RISCV, chip RP2350
UF2_FAMILY_ABSOLUTE = 0xe48bff57


def fail(msg):
    sys.exit(f"add_riscv_twin: {msg}")


def word(b, o):
    return struct.unpack_from('<I', b, o)[0]


def parse_block(b, o):
    """Returns (offset of the link word, number of leading words a HASH_DEF covers, or 0)."""
    p, hashed = o + 4, 0
    while True:
        h = word(b, p)
        t = h & 0xff
        if t == ITEM_LAST:
            if word(b, p + 8) != MARKER_END:
                fail(f"block at {o:#x} has no end marker")
            return p + 4, hashed
        size = (h >> 8) & (0xffff if t & 0x80 else 0xff)
        if size == 0:
            fail(f"block at {o:#x}: empty item {t:#x}")
        if t == ITEM_HASH_DEF:
            hashed = word(b, p + 4) & 0xffff   # block words included in the hash
        p += 4 * size


def main():
    if len(sys.argv) != 6:
        fail("usage: add_riscv_twin.py <arm.bin> <twin.bin> <twin_offset> <out.bin> <out.uf2>")
    arm = bytearray(open(sys.argv[1], 'rb').read())
    twin = bytearray(open(sys.argv[2], 'rb').read())
    twin_off = int(sys.argv[3], 0)
    if len(arm) > twin_off:
        fail(f"the ARM stub ({len(arm):#x} bytes) runs into the RISC-V half at {twin_off:#x}")

    first = next((o for o in range(0, min(len(arm), 0x1000), 4) if word(arm, o) == MARKER_START), None)
    if first is None:
        fail("no block in the ARM stub's first 4 KB")
    # walk the loop to the block that links back to the first one
    o, seen = first, set()
    while True:
        if o in seen or not 0 <= o < len(arm) or word(arm, o) != MARKER_START:
            fail(f"broken block loop at {o:#x}")
        seen.add(o)
        link_at, hashed = parse_block(arm, o)
        nxt = o + struct.unpack_from('<i', arm, link_at)[0]
        if nxt == first:
            break
        o = nxt
    last, last_link_at = o, link_at
    if hashed and (last_link_at - last) // 4 <= hashed:   # counted from the marker or after it
        fail(f"the link of the block at {last:#x} is covered by its hash")

    twin += b'\0' * (-len(twin) % 4)
    block_off = twin_off + len(twin)
    block = struct.pack('<IIIIII',
                        MARKER_START,
                        ITEM_IMAGE_TYPE | 1 << 8 | IMAGE_TYPE_RISCV_EXE << 16,
                        ITEM_ENTRY_POINT | 3 << 8, XIP_BASE + twin_off, TWIN_SP,
                        ITEM_LAST | 4 << 8)    # LAST: 4 words of items
    block += struct.pack('<iI', first - block_off, MARKER_END)
    if block_off + len(block) > STUB_SIZE:
        fail(f"the RISC-V half ends at {block_off + len(block):#x}, past {STUB_SIZE:#x}")
    struct.pack_into('<i', arm, last_link_at, block_off - last)

    image = arm + b'\xff' * (twin_off - len(arm)) + twin + block
    image += b'\xff' * (-len(image) % 256)
    open(sys.argv[4], 'wb').write(image)

    n = len(image) // 256
    with open(sys.argv[5], 'wb') as f:
        for i in range(n):
            hdr = struct.pack('<IIIIIIII', 0x0a324655, 0x9e5d5157, 0x2000,
                              XIP_BASE + 256 * i, 256, i, n, UF2_FAMILY_ABSOLUTE)
            f.write(hdr + image[256 * i:256 * (i + 1)] + b'\0' * 220 + struct.pack('<I', 0x0ab16f30))
    print(f"add_riscv_twin: RISC-V half at {twin_off:#x}, block at {block_off:#x} "
          f"(linked after {last:#x}), {len(image)} bytes")


if __name__ == '__main__':
    main()
