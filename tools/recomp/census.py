"""Instruction census of the game's code: decode every word inside every function in functions.csv.

Usage:
    python3 tools/recomp/census.py orig/0005000010143500_v0/code/cking.rpx [--csv OUT.csv]

Prints each mnemonic with its count, then any word that fails to decode (with its function) and
any mnemonic listed in ppc.NOTES (where the M1 oracle, Cemu's interpreter, differs). Exit status 1
if anything failed to decode. Standard library only; prints counts and addresses, no game bytes.
"""
import collections
import csv
import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import ppc  # noqa: E402
from rpx_info import Rpx  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[2]


def text_section(rpx):
    for i, s in enumerate(rpx.sh):
        if rpx.name(i) == ".text":
            return s[3], rpx.data(i)
    sys.exit("no .text section")


def functions(path=ROOT / "config/US_v0/functions.csv"):
    with open(path) as f:
        for row in csv.DictReader(f):
            yield int(row["address"], 16), int(row["end"], 16), row["name"]


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    base, text = text_section(Rpx(args[0]))
    counts = collections.Counter()
    bad = []
    outside = 0
    for start, end, name in functions():
        if not base <= start < base + len(text):
            outside += 1        # import stubs in the RPL import sections: no code of ours
            continue
        for ea in range(start, end, 4):
            w, = struct.unpack_from(">I", text, ea - base)
            insn = ppc.decode(w)
            if insn is None:
                bad.append((ea, name))
            else:
                counts[insn.op] += 1
    total = sum(counts.values())
    print(f"{total} instructions, {len(counts)} mnemonics, {len(bad)} undecodable "
          f"({outside} functions outside .text skipped)")
    for op, n in counts.most_common():
        note = ppc.NOTES.get(op)
        print(f"  {op:12} {n:9}" + (f"   ! {note}" if note else ""))
    for ea, name in bad[:50]:
        print(f"  undecodable at {ea:08X} in {name}")
    if "--csv" in args:
        with open(args[args.index("--csv") + 1], "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["mnemonic", "count"])
            w.writerows(sorted(counts.items()))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
