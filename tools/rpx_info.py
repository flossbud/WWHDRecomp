"""Summarise a Wii U .rpx/.rpl: sections, imports, relocations, source-file strings.

Usage:
    python3 tools/rpx_info.py orig/0005000010143500_v0/code/cking.rpx [--sources]

Standard library only. RPX sections flagged 0x08000000 (SHF_RPL_ZLIB) are
stored as a u32 decompressed size followed by a zlib stream.
"""
import collections
import re
import struct
import sys
import zlib

SHF_RPL_ZLIB = 0x08000000
SECTION_TYPES = {
    1: "PROGBITS", 2: "SYMTAB", 3: "STRTAB", 4: "RELA", 8: "NOBITS",
    0x80000001: "RPL_EXPORTS", 0x80000002: "RPL_IMPORTS",
    0x80000003: "RPL_CRCS", 0x80000004: "RPL_FILEINFO",
}
RELOC_NAMES = {1: "ADDR32", 4: "ADDR16_LO", 5: "ADDR16_HI", 6: "ADDR16_HA", 10: "REL24", 11: "REL14"}


class Rpx:
    def __init__(self, path):
        self.raw = open(path, "rb").read()
        if self.raw[:4] != b"\x7fELF" or self.raw[7:9] != b"\xca\xfe":
            sys.exit(f"{path}: not a Cafe (Wii U) ELF")
        shoff, = struct.unpack(">I", self.raw[0x20:0x24])
        shnum, shstrndx = struct.unpack(">HH", self.raw[0x30:0x34])
        # (name, type, flags, addr, offset, size, link, info, addralign, entsize)
        self.sh = [struct.unpack(">10I", self.raw[shoff + i * 40:shoff + i * 40 + 40]) for i in range(shnum)]
        self.shstr = self.data(shstrndx)

    def data(self, i):
        _n, typ, flags, _a, off, size, *_ = self.sh[i]
        if typ == 8:
            return b""
        blob = self.raw[off:off + size]
        return zlib.decompress(blob[4:]) if flags & SHF_RPL_ZLIB else blob

    def name(self, i):
        o = self.sh[i][0]
        return self.shstr[o:self.shstr.index(b"\0", o)].decode()

    def size(self, i):
        return self.sh[i][5] if self.sh[i][1] == 8 else len(self.data(i))


def main():
    rpx = Rpx(sys.argv[1])
    print("== sections")
    for i, s in enumerate(rpx.sh[1:], 1):
        kind = SECTION_TYPES.get(s[1], hex(s[1]))
        print(f"{i:3d} {rpx.name(i):24s} {kind:13s} addr {s[3]:08x} size {rpx.size(i):8x}")

    print("\n== imported libraries")
    for i, s in enumerate(rpx.sh):
        if s[1] == 0x80000002 and rpx.name(i).startswith(".fimport_"):
            print(f"  {rpx.name(i)[9:]}")
    internal = 0
    for i, s in enumerate(rpx.sh):
        if s[1] == 2:
            st = rpx.data(i)
            for j in range(0, len(st), 16):
                info, shndx = st[j + 12], struct.unpack(">H", st[j + 14:j + 16])[0]
                if info & 0xF in (1, 2) and 0 < shndx < len(rpx.sh) and rpx.sh[shndx][1] in (1, 8):
                    internal += 1
    print(f"  func/object symbols defined in the module itself: {internal} (0 => stripped)")

    print("\n== relocations")
    counts = collections.Counter()
    for i, s in enumerate(rpx.sh):
        if s[1] == 4:
            r = rpx.data(i)
            for j in range(0, len(r), 12):
                counts[struct.unpack(">I", r[j + 4:j + 8])[0] & 0xFF] += 1
    for t, n in sorted(counts.items()):
        print(f"  {RELOC_NAMES.get(t, t):10s} {n}")

    ro = b"".join(rpx.data(i) for i, s in enumerate(rpx.sh) if rpx.name(i) in (".rodata", ".data"))
    sources = sorted({m.group(1).decode() for m in re.finditer(rb"([A-Za-z0-9_]+\.(?:cpp|c))\b", ro)})
    print(f"\n== {len(sources)} distinct source-file names in .rodata/.data "
          f"({sum(s.startswith('d_a_') for s in sources)} are d_a_* actors)")
    if "--sources" in sys.argv:
        print("\n".join(f"  {s}" for s in sources))


if __name__ == "__main__":
    main()
