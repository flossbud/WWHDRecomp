"""Recover GHS switch jump tables from an RPX and write config/US_v0/jump_tables.csv.

GHS does not put switch tables in data. It computes an address inside a table
of `b case_N` instructions placed directly after the bctr:

    cmplwi  rI, N            ; bound (often in a predecessor block)
    bgt     default
    lis     rT, table@ha     ; or: slwi rI,rI,2 / addis rT,rI,table@ha
    slwi    rI, rI, 2
    addic   rT, rT, table@l  ; or addi
    add     rT, rI, rT
    mtctr   rT
    bctr
  table:
    b case_0
    b case_1 ...

Ghidra's switch analysis does not recognise this, and the lis/addic relocation
makes the table look like a function entry. The CSV feeds
tools/ghidra/apply_jump_tables.py and the recompiler.

Usage:
    python3 tools/jump_tables.py [orig/.../cking.rpx] [--out config/US_v0/jump_tables.csv]

Columns: bctr, table, count, bound (cmplwi | b-run), targets (space-separated).
`b-run` means no cmplwi bound was found and the count is the run of consecutive
`b` instructions at the table, which can over-count by the length of any
following code that also starts with `b`.
"""
import argparse
import csv
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rpx_info import Rpx  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
BCTR = 0x4E800420
BLR = 0x4E800020
BOUND_SEARCH = 64  # instructions to look back for the cmplwi


def sext(value, bits):
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


class Text:
    def __init__(self, rpx):
        i = next(i for i in range(len(rpx.sh)) if rpx.name(i) == ".text")
        self.base = rpx.sh[i][3]
        self.data = rpx.data(i)
        self.end = self.base + len(self.data)

    def word(self, addr):
        return struct.unpack_from(">I", self.data, addr - self.base)[0]

    def __contains__(self, addr):
        return self.base <= addr < self.end


def is_b(w):
    return w >> 26 == 18 and w & 3 == 0


def b_target(addr, w):
    return (addr + sext(w & 0x03FFFFFC, 26)) & 0xFFFFFFFF


def find_table(text, bctr):
    """Return the table address if the block ending at bctr computes one with lis/addi."""
    block = []
    for k in range(1, 14):
        a = bctr - 4 * k
        if a not in text:
            break
        w = text.word(a)
        if w == BLR or w >> 26 == 18:
            break
        block.append(w)
    hi, table = {}, None
    for w in reversed(block):
        op, rd, ra, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        if op == 15:                                   # lis rT,hi / addis rT,rI,hi
            hi[rd] = imm << 16
        elif op in (12, 14) and ra in hi:              # addic / addi rT,rT,lo
            table = (hi[ra] + sext(imm, 16)) & 0xFFFFFFFF
    return table if table is not None and table in text else None


def slwi_source(text, bctr):
    """The register shifted left by 2 in the switch block (the case index, before scaling)."""
    for k in range(1, 14):
        w = text.word(bctr - 4 * k)
        # slwi rA, rS, 2  ==  rlwinm rA, rS, 2, 0, 29
        if w >> 26 == 21 and (w >> 11) & 31 == 2 and (w >> 6) & 31 == 0 and (w >> 1) & 31 == 29:
            return (w >> 21) & 31
    return None


def find_bound(text, bctr, reg):
    """Search back for `cmplwi reg, N` followed by bgt/bge; return entry count or None."""
    if reg is None:
        return None
    count = None
    for k in range(1, BOUND_SEARCH):
        a = bctr - 4 * k
        if a not in text:
            break
        w = text.word(a)
        if w >> 26 == 10 and not (w >> 21) & 1 and (w >> 16) & 31 == reg:
            crf = (w >> 23) & 7
            # the conditional branch using this compare follows it
            for j in range(1, 6):
                bw = text.word(a + 4 * j)
                if bw >> 26 != 16:
                    continue
                bo, bi = (bw >> 21) & 31, (bw >> 16) & 31
                if bi // 4 != crf:
                    continue
                if bo == 12 and bi % 4 == 1:
                    count = (w & 0xFFFF) + 1       # bgt default
                elif bo == 4 and bi % 4 == 0:
                    count = w & 0xFFFF             # bge default
                break
            if count is not None:
                return count
    return None


def recover(rpx_path):
    text = Text(Rpx(rpx_path))
    rows = []
    for bctr in range(text.base, text.end, 4):
        if text.word(bctr) != BCTR:
            continue
        table = find_table(text, bctr)
        if table is None:
            continue
        run = 0
        while table + 4 * run in text and is_b(text.word(table + 4 * run)):
            run += 1
        if run == 0:
            continue
        count = find_bound(text, bctr, slwi_source(text, bctr))
        bound = "cmplwi"
        if count is None or count > run:
            count, bound = run, "b-run"
        targets = [b_target(table + 4 * i, text.word(table + 4 * i)) for i in range(count)]
        rows.append((bctr, table, count, bound, targets))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rpx", nargs="?", default=ROOT / "orig" / "0005000010143500_v0" / "code" / "cking.rpx")
    ap.add_argument("--out", default=ROOT / "config" / "US_v0" / "jump_tables.csv")
    args = ap.parse_args()
    rows = recover(args.rpx)
    with open(args.out, "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["bctr", "table", "count", "bound", "targets"])
        for bctr, table, count, bound, targets in rows:
            w.writerow([f"{bctr:08X}", f"{table:08X}", count, bound, " ".join(f"{t:08X}" for t in targets)])
    by_bound = {b: sum(1 for r in rows if r[3] == b) for b in ("cmplwi", "b-run")}
    print(f"{len(rows)} switches ({by_bound['cmplwi']} bounded by cmplwi, {by_bound['b-run']} by b-run), "
          f"{sum(r[2] for r in rows)} cases -> {args.out}")


if __name__ == "__main__":
    main()
