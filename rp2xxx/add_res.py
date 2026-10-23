#!/usr/bin/env python3
# add_res.py - append the loader's flash resources to pml2350.uf2.
#
#   add_res.py <res.bin> <base> <loader.uf2>
#
# <res.bin> is the ELF's .pml_res section (the Wi-Fi firmware and the web page, linked at <base>, the
# last 256 KB of flash): a 16-byte header (magic, size, crc, check), then the data. This fills in the
# header and appends the section to the UF2 as blocks of their own family. The boot stub only loads
# RP2350 ARM blocks into RAM and skips these; the loader copies them into flash itself (uf2_image.c).
# The header also goes into the loader's RAM image, over a marker (s_res_want), so the loader knows
# which resources it was linked with.
import struct
import sys
import zlib

RES_MAGIC = 0x52574d50          # "PMWR"
RES_FAMILY = 0x504d4c52         # "RLMP": PicoMultiLoader resources, ignored by everything else
RES_SIZE = 256 * 1024
UF2_MAGIC0, UF2_MAGIC1, UF2_MAGIC_END = 0x0A324655, 0x9E5D5157, 0x0AB16F30
UF2_FLAG_FAMILY = 0x2000
WANT_MARKER = struct.pack('<4I', 0x4d4c4d50, 0x57534552, 0x5f544e41, 0x52444448)  # s_res_want


def fail(msg):
    sys.exit(f"add_res: {msg}")


def main():
    if len(sys.argv) != 4:
        fail("usage: add_res.py <res.bin> <base> <loader.uf2>")
    res = bytearray(open(sys.argv[1], 'rb').read())
    base = int(sys.argv[2], 0)
    uf2_path = sys.argv[3]

    if len(res) < 16 or struct.unpack_from('<I', res, 0)[0] != RES_MAGIC:
        fail("the .pml_res section does not start with the header")
    if len(res) > RES_SIZE:
        fail(f"resources are {len(res)} bytes, more than {RES_SIZE}")
    payload = bytes(res[16:])
    size, crc = len(payload), zlib.crc32(payload)
    struct.pack_into('<IIII', res, 0, RES_MAGIC, size, crc, RES_MAGIC ^ size ^ crc ^ 0xffffffff)
    res += b'\xff' * (-len(res) % 256)

    # Keep the loader's own blocks, drop resource blocks from an earlier run
    uf2 = open(uf2_path, 'rb').read()
    if len(uf2) % 512:
        fail(f"{uf2_path} is not a whole number of UF2 blocks")
    kept = bytearray()
    for o in range(0, len(uf2), 512):
        b = uf2[o:o + 512]
        m0, m1, flags, _, _, _, _, fam = struct.unpack_from('<8I', b, 0)
        if m0 != UF2_MAGIC0 or m1 != UF2_MAGIC1 or struct.unpack_from('<I', b, 508)[0] != UF2_MAGIC_END:
            fail(f"bad UF2 block at offset {o}")
        if not (flags & UF2_FLAG_FAMILY and fam == RES_FAMILY):
            kept += b

    # The marker is 16-byte aligned, so it never straddles two blocks' 256-byte payloads
    found = [o for o in range(0, len(kept), 512) if WANT_MARKER in kept[o + 32:o + 288]]
    if len(found) != 1:
        fail(f"found the s_res_want marker {len(found)} times, expected once")
    at = kept.find(WANT_MARKER, found[0] + 32)
    kept[at:at + 16] = res[0:16]

    n = len(res) // 256
    for i in range(n):
        block = struct.pack('<8I', UF2_MAGIC0, UF2_MAGIC1, UF2_FLAG_FAMILY, base + i * 256, 256, i, n, RES_FAMILY)
        block += res[i * 256:(i + 1) * 256] + b'\0' * 220 + struct.pack('<I', UF2_MAGIC_END)
        kept += block
    open(uf2_path, 'wb').write(kept)
    print(f"add_res: {size + 16} bytes of resources at 0x{base:08x} (crc {crc:08x}) added to {uf2_path}")


if __name__ == '__main__':
    main()
