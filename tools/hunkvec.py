# SPDX-License-Identifier: MPL-2.0
"""List the library vector table of an AmigaOS hunk-format library.

Reads hunk contents (hunk 0 = code, hunk 1 = data), follows the ROMTag to the
init table and prints (LVO, hunk, offset) for each vector. Offsets are as
stored; relocation targets are taken from HUNK_RELOC32 blocks.
"""
import struct
import sys

HUNK_HEADER, HUNK_CODE, HUNK_DATA, HUNK_BSS = 0x3F3, 0x3E9, 0x3EA, 0x3EB
HUNK_RELOC32, HUNK_END, HUNK_SYMBOL, HUNK_DEBUG = 0x3EC, 0x3F2, 0x3F0, 0x3F1


def parse(buf):
    pos = 0

    def u32():
        nonlocal pos
        v = struct.unpack_from(">I", buf, pos)[0]
        pos += 4
        return v

    assert u32() == HUNK_HEADER
    while True:
        n = u32()
        if n == 0:
            break
        pos += n * 4
    nh = u32()
    first = u32()
    last = u32()
    pos += (last - first + 1) * 4
    hunks = []
    cur = None
    while pos < len(buf):
        t = u32() & 0x3FFFFFFF
        if t in (HUNK_CODE, HUNK_DATA):
            n = u32()
            cur = {"data": bytearray(buf[pos:pos + n * 4]), "reloc": {}}
            pos += n * 4
            hunks.append(cur)
        elif t == HUNK_BSS:
            n = u32()
            cur = {"data": bytearray(n * 4), "reloc": {}}
            hunks.append(cur)
        elif t == HUNK_RELOC32:
            while True:
                n = u32()
                if n == 0:
                    break
                target = u32()
                for _ in range(n):
                    off = u32()
                    cur["reloc"][off] = target
        elif t == 0x3F7:
            # RELOC32SHORT: 16-bit words: count, target, offsets...; ends at
            # a zero count, then padded to a long boundary.
            while True:
                n = struct.unpack_from(">H", buf, pos)[0]
                pos += 2
                if n == 0:
                    break
                target = struct.unpack_from(">H", buf, pos)[0]
                pos += 2
                for _ in range(n):
                    off = struct.unpack_from(">H", buf, pos)[0]
                    pos += 2
                    cur["reloc"][off] = target
            pos = (pos + 3) & ~3
        elif t == HUNK_END:
            pass
        elif t in (HUNK_SYMBOL, HUNK_DEBUG):
            if t == HUNK_DEBUG:
                n = u32()
                pos += n * 4
            else:
                while True:
                    n = u32()
                    if n == 0:
                        break
                    pos += n * 4 + 4
        else:
            raise SystemExit("unknown hunk %x at %x" % (t, pos))
    return hunks


def main():
    buf = open(sys.argv[1], "rb").read()
    hunks = parse(buf)
    code = hunks[0]["data"]
    i = 0
    while True:
        i = code.find(b"\x4a\xfc", i)
        if i < 0:
            raise SystemExit("no ROMTag")
        if struct.unpack_from(">I", code, i + 2)[0] == i:
            break
        i += 2
    name_off = struct.unpack_from(">I", code, i + 14)[0]
    init_off = struct.unpack_from(">I", code, i + 22)[0]
    print("romtag at", hex(i), "flags", code[i + 10], "init", hex(init_off))
    # init table: size, funcTable, dataInit, initFunc (all relocated offsets)
    hid = hunks[0]["reloc"].get(i + 22, 0)
    base = hunks[hid]["data"]
    size, ftab, dinit, ifn = struct.unpack_from(">IIII", base, init_off)
    ft_hunk = hunks[hid]["reloc"].get(init_off + 4, 0)
    print("base size", size, "functable hunk", ft_hunk, "off", hex(ftab))
    fdata = hunks[ft_hunk]["data"]
    n = 0
    fn_off = ftab
    while True:
        v = struct.unpack_from(">I", fdata, fn_off)[0]
        if v == 0xFFFFFFFF:
            break
        tgt = hunks[ft_hunk]["reloc"].get(fn_off, None)
        # LVO index n: first user function (index 5) is at -30
        print("%4d  LVO -%d  hunk %s off 0x%x" % (n + 1, (n + 1) * 6, tgt, v))
        n += 1
        fn_off += 4
    print("entries:", n)


main()
