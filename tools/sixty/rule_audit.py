"""Audit the tick rules for a store skipped on half ticks whose value the code still uses (D21).

Usage (on the worker; needs orig/):
    python3 tools/sixty/rule_audit.py [--after N]

A `whole` or `late` rule on a store skips only the store: on a half tick the register still holds the
computed value (a count + 1, a timer - 1), and code that goes on to test it, return it or set cr0 from
it acts on the next tick's value half a tick early, and again on the next whole tick (the shared
countdown helpers f_02055B64 and f_0211D2F8 returned their register: callers waiting for 0 acted
twice). Such a count wants `keep` on its add instead (the half tick computes nothing new), or `whole`
on what it triggers. For each such rule this lists the store and the first use of its register in
the next N instructions (default 8) or a record form (`extsh.`, `rlwinm.`, ...) of it just before.
The output names the game's instructions: keep it on the worker.
"""
import argparse
import glob
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools/recomp"))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import ppc  # noqa: E402
from rpx_info import Rpx  # noqa: E402

STORES = {"stw", "sth", "stb", "stwx", "sthx", "stbx"}
COMPARES = {"cmpi", "cmpli", "cmp", "cmpl"}       # the decoder's names (cmpwi is cmpi)


def rules():
    files = [os.path.join(ROOT, "config/US_v0/tick_rules.txt")] + sorted(glob.glob(os.path.join(ROOT, "config/US_v0/tick_rules/*.txt")))
    for path in files:
        for n, line in enumerate(open(path), 1):
            words = line.split("#", 1)[0].split()
            if len(words) >= 3 and words[1].split(":")[0] in ("whole", "late") and words[2] in STORES:
                yield os.path.relpath(path, ROOT), n, int(words[0], 16), words[1], " ".join(words[3:])[:60]


def reads(i, reg):
    """Whether instruction i reads GPR reg (the forms the audit cares about)."""
    f = i.f
    if i.op in COMPARES:                               # cmpi/cmpli: rB is part of the immediate
        return f.get("rA") == reg or (i.op in ("cmp", "cmpl") and f.get("rB") == reg)
    if i.op == "or":                                   # mr rA, rS
        return f.get("rT") == reg or f.get("rB") == reg
    if i.op.startswith(("l", "st", "psq")):             # loads and stores: reg as an address only
        return (f.get("rA") == reg and reg != 0) or f.get("rB") == reg
    if i.op in ("addi", "addis") and f.get("rA") == 0:  # li, lis
        return False
    return any(f.get(k) == reg for k in ("rA", "rB", "rS", "rT"))


def writes(i, reg):
    f = i.f
    if i.op in ("or", "extsh", "extsb", "rlwinm", "andi.", "ori", "xori"):
        return f.get("rA") == reg
    return f.get("rD") == reg or (i.op in ("lwz", "lhz", "lha", "lbz", "lwzx", "lhzx", "lhax", "lbzx") and f.get("rT", f.get("rD")) == reg)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--after", type=int, default=8)
    args = ap.parse_args()
    rpx = Rpx(os.path.join(ROOT, "orig/0005000010143500_v0/code/cking.rpx"))
    code = [(rpx.sh[i][3], rpx.data(i)) for i in range(len(rpx.sh)) if rpx.name(i) in (".text", ".syscall")]

    def word(ea):
        for base, data in code:
            if base <= ea < base + len(data):
                return struct.unpack_from(">I", data, ea - base)[0]
        raise IndexError(ea)

    for path, n, ea, rule, what in rules():
        st = ppc.decode(word(ea))
        reg = st.f.get("rT")
        if reg is None:
            continue
        found = None
        prev = ppc.decode(word(ea - 4))                # a record form just before (extsh. r12,r12; sth r12)
        if prev is not None and prev.f.get("rc") and prev.f.get("rA") == reg:
            nxt = ppc.decode(word(ea + 4))
            if nxt is not None and nxt.op == "bc":
                found = f"{prev.op}. sets cr0 before the store, {ea + 4:08X} branches on it"
        for k in range(1, args.after + 1):
            if found:
                break
            i = ppc.decode(word(ea + 4 * k))
            if i is None:
                break
            if reads(i, reg):
                found = f"{ea + 4 * k:08X} {i.op} reads r{reg}"
                break
            if writes(i, reg) or i.op == "bclr" or (i.op == "b" and not i.f.get("lk")):
                break
        if found:
            print(f"{path}:{n}  {ea:08X} {rule:6s} {st.op} r{reg}  {what}\n    {found}")


if __name__ == "__main__":
    main()
