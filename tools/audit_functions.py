"""Sanity-check config/US_v0/functions.csv against the RPX's own facts.

Checks:
  * no function starts at a switch-table entry (jump_tables.csv); a function at
    a case label is only legitimate when the case is a tail call, i.e. the
    target looks like a real function (prologue, or called/address-taken
    elsewhere)
  * relocation targets in .text (address-taken code) that land *inside* a
    function body rather than at its entry, split by what precedes them:
      after a terminator  - previous instruction is b/blr/bctr, so nothing falls
                            through: almost certainly a separate function that
                            got merged into its neighbour (a tail-call target)
      after fallthrough   - a genuine mid-function label, or a merge we can't
                            prove from the bytes alone
  * function starts GHS could not have produced: code falls through into them
    (previous word is not a terminator, zero or nop padding) and nothing marks
    them as an entry (no prologue, not called or tail-called, not address-taken). The GHS
    register save/restore helpers are the known exception: one straight run of
    stw/lwz/stfd/lfd with an entry per register, called part-way in, so each
    entry falls through into the next.
  * functions whose body is not whole instructions (size not a multiple of 4):
    Ghidra made a function at bytes it never disassembled
  * .text bytes not covered by any function
  * non-contiguous bodies

Usage:
    python3 tools/audit_functions.py [--list]
"""
import argparse
import bisect
import csv
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from jump_tables import Text  # noqa: E402
from rpx_info import Rpx  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
CONFIG = ROOT / "config" / "US_v0"
RPX = ROOT / "orig" / "0005000010143500_v0" / "code" / "cking.rpx"
ADDRESS_RELOCS = {1, 4, 6}


PROLOGUE = (0x7C0802A6,)  # mflr r0
PADDING = (0, 0x60000000)  # zero, nop


def looks_like_entry(w):
    return w in PROLOGUE or w >> 16 == 0x9421  # stwu r1, -N(r1)


def terminates(w):
    """b (no link), blr, bctr: control never falls through to the next word."""
    return (w >> 26 == 18 and w & 1 == 0) or w in (0x4E800020, 0x4E800420)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list", action="store_true", help="print every finding")
    args = ap.parse_args()

    rpx = Rpx(RPX)
    text = Text(rpx)
    funcs = sorted((int(r["address"], 16), int(r["end"], 16), int(r["size"]), int(r["ranges"]), r["name"])
                   for r in csv.DictReader(open(CONFIG / "functions.csv")) if int(r["address"], 16) in text)
    starts = [f[0] for f in funcs]
    entries, cases = set(), set()
    for r in csv.DictReader(open(CONFIG / "jump_tables.csv")):
        table = int(r["table"], 16)
        entries.update(table + 4 * i for i in range(int(r["count"])))
        cases.update(int(t, 16) for t in r["targets"].split())
    switch = entries | cases

    def containing(a):
        i = bisect.bisect_right(starts, a) - 1
        return funcs[i] if i >= 0 and a < funcs[i][1] else None

    targets = {t for _s, _o, typ, t in rpx.relocations() if typ in ADDRESS_RELOCS and t in text and t % 4 == 0}
    called = set()  # targets of bl, and of b from outside the target's own function
    branched = {}
    for a in range(text.base, text.end, 4):
        w = text.word(a)
        if w >> 26 == 18 and w & 2 == 0:
            dest = (a + ((w & 0x03FFFFFC) ^ 0x02000000) - 0x02000000) & 0xFFFFFFFF
            if w & 1:
                called.add(dest)
            else:
                branched.setdefault(dest, []).append(a)
    for dest, sources in branched.items():
        f = containing(dest)
        if f is not None and f[0] == dest and any(not (f[0] <= s < f[1]) for s in sources):
            called.add(dest)  # tail call, e.g. an epilogue branching into a restore helper
    table_entries = [f for f in funcs if f[0] in entries]
    case_funcs = [f for f in funcs if f[0] in cases]
    tail_cases = [f for f in case_funcs
                  if looks_like_entry(text.word(f[0])) or f[0] in called or f[0] in targets]
    label_cases = [f for f in case_funcs if f not in tail_cases]
    inside = [(t, containing(t)) for t in sorted(targets - set(starts) - switch)]
    inside = [(t, f) for t, f in inside if f is not None]
    after_term = [(t, f) for t, f in inside if terminates(text.word(t - 4))]
    after_fall = [(t, f) for t, f in inside if not terminates(text.word(t - 4))]
    covered = sum(f[2] for f in funcs)
    early = [f for f in funcs if f[1] - f[0] != f[2] and f[3] == 1]  # body extends before entry
    implausible = [f for f in funcs
                   if f[0] % 4 or (f[0] != text.base and not terminates(text.word(f[0] - 4))
                                   and text.word(f[0] - 4) not in PADDING and not looks_like_entry(text.word(f[0]))
                                   and f[0] not in targets and f[0] not in called)]

    print(f"functions in .text:                 {len(funcs)}")
    print(f".text covered:                      {covered:#x} / {text.end - text.base:#x} "
          f"({100 * covered / (text.end - text.base):.2f}%)")
    print(f"non-contiguous bodies:              {sum(1 for f in funcs if f[3] > 1)}")
    print(f"bodies that aren't whole insns:     {sum(1 for f in funcs if f[2] % 4 or f[0] % 4)}")
    print(f"bodies starting before their entry: {len(early)}")
    print(f"implausible function starts:        {len(implausible)}")
    print(f"functions at switch-table entries:  {len(table_entries)}")
    print(f"functions at case labels:           {len(case_funcs)}")
    print(f"  tail-call cases (real functions): {len(tail_cases)}")
    print(f"  plain labels (wrong):             {len(label_cases)}")
    print(f"address-taken code inside a body:   {len(inside)}")
    print(f"  after a terminator (merged fn?):  {len(after_term)}")
    print(f"  after fallthrough (label?):       {len(after_fall)}")
    if args.list:
        for f in implausible:
            print(f"implausible   {f[0]:08X} {f[4]}")
        for f in table_entries:
            print(f"table-entry   {f[0]:08X} {f[4]}")
        for f in label_cases:
            print(f"case-label    {f[0]:08X} {f[4]}")
        for kind, rows in (("merged?", after_term), ("label?", after_fall)):
            for t, f in rows:
                print(f"{kind:12s}  {t:08X} inside {f[4]} [{f[0]:08X}..{f[1]:08X})")


if __name__ == "__main__":
    main()
