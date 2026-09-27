# /// script
# requires-python = ">=3.10"
# dependencies = ["zstandard"]
# ///
"""Extract files from a Cemu .wua (ZArchive) without building Exzap/ZArchive.

Usage:
    uv run tools/wua_extract.py ARCHIVE.wua                  # list contents
    uv run tools/wua_extract.py ARCHIVE.wua OUTDIR [FILTER…] # extract

FILTER is a path fragment matched against the path inside the title folder,
e.g. `code/` or `meta/meta.xml`. No filter extracts everything.

Format reference: https://github.com/Exzap/ZArchive (zarchivecommon.h).
"""
import hashlib
import os
import struct
import sys

import zstandard

MAGIC = 0x169F52D6
FOOTER_SIZE = 6 * 16 + 32 + 8 + 4 + 4
BLOCK_SIZE = 64 * 1024
BLOCKS_PER_RECORD = 16
RECORD_SIZE = 8 + 2 * BLOCKS_PER_RECORD

KNOWN_HASHES = {
    "0005000010143500_v0/code/cking.rpx":
        "c4f0ab300542e0bfc462696850534e71db2ad02288a7eb55e5a4cd4062f16153",
}


class ZArchive:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.f.seek(0, os.SEEK_END)
        size = self.f.tell()
        self.f.seek(size - FOOTER_SIZE)
        footer = self.f.read(FOOTER_SIZE)
        self.sections = [struct.unpack(">QQ", footer[i * 16:i * 16 + 16]) for i in range(6)]
        total, _version, magic = struct.unpack(">QII", footer[128:])
        if magic != MAGIC or total != size:
            sys.exit(f"{path}: not a ZArchive (.wua) file")
        self.offset_records = self._read(1)
        self.names = self._read(2)
        tree = self._read(3)
        self.entries = [struct.unpack(">IIIHH", tree[i:i + 16]) for i in range(0, len(tree), 16)]
        self.dctx = zstandard.ZstdDecompressor()

    def _read(self, idx):
        off, size = self.sections[idx]
        self.f.seek(off)
        return self.f.read(size)

    def _name(self, off):
        b0 = self.names[off]
        if b0 & 0x80:
            length = (b0 & 0x7F) | (self.names[off + 1] << 7)
            off += 2
        else:
            length = b0
            off += 1
        return self.names[off:off + length].decode()

    def files(self, idx=0, prefix=""):
        """Yield (path, offset, size) for every file under directory entry idx."""
        type_name, a, b, c, d = self.entries[idx]
        name = self._name(type_name & 0x7FFFFFFF) if idx else ""
        if type_name >> 31:
            yield prefix + name, a | (d << 32), b | (c << 32)
            return
        for child in range(a, a + b):
            yield from self.files(child, prefix + name + ("/" if idx else ""))

    def _block(self, n):
        rec = self.offset_records[(n // BLOCKS_PER_RECORD) * RECORD_SIZE:][:RECORD_SIZE]
        base = struct.unpack(">Q", rec[:8])[0]
        sizes = struct.unpack(f">{BLOCKS_PER_RECORD}H", rec[8:])
        i = n % BLOCKS_PER_RECORD
        self.f.seek(self.sections[0][0] + base + sum(s + 1 for s in sizes[:i]))
        raw = self.f.read(sizes[i] + 1)
        if len(raw) == BLOCK_SIZE:  # stored uncompressed
            return raw
        return self.dctx.decompress(raw, max_output_size=BLOCK_SIZE)

    def read_file(self, offset, size):
        out = bytearray()
        pos = offset
        while len(out) < size:
            block = self._block(pos // BLOCK_SIZE)
            start = pos % BLOCK_SIZE
            chunk = block[start:start + min(BLOCK_SIZE - start, size - len(out))]
            out += chunk
            pos += len(chunk)
        return bytes(out)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    arc = ZArchive(sys.argv[1])
    if len(sys.argv) == 2:
        for path, _off, size in arc.files():
            print(f"{size:12d}  {path}")
        return
    outdir, filters = sys.argv[2], sys.argv[3:]
    for path, off, size in arc.files():
        inner = path.split("/", 1)[1] if "/" in path else path
        if filters and not any(inner.startswith(f) for f in filters):
            continue
        data = arc.read_file(off, size)
        dst = os.path.join(outdir, path)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as out:
            out.write(data)
        note = ""
        if path in KNOWN_HASHES:
            ok = hashlib.sha256(data).hexdigest() == KNOWN_HASHES[path]
            note = "  [sha256 OK]" if ok else "  [sha256 MISMATCH - unexpected build]"
        print(f"{size:12d}  {dst}{note}")


if __name__ == "__main__":
    main()
