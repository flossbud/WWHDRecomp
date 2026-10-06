"""Candidates for the `eqwhole` step rule (tools/recomp/generate.py): equality tests of counts kept to whole ticks.

A count a tick rule keeps to whole ticks (`whole` or `keep` on its +-1, e.g. a timer counting down) reads the same
on a stepping process's whole step and on the half step after it, so `if (timer == 40) ...` fires twice a tick
at 60 (Gohma's armour cracked after two rocks, not three). `eqwhole` on the compare makes it read unequal on the
half step. This lists the compares to put it on.

Usage (on a worker; disasm.py's output is the game's code, so it stays there):
    uv run tools/ghidra/disasm.py FUNC... --out DIR          # the process's functions (f_XXXXXXXX.s files)
    python3 tools/sixty/eqsites.py DIR FIELD... [--rules TAG]

FIELD: the counts' hex offsets in the process (the ones its rules keep to whole ticks: check the rule file;
a `late` count changes on the half step and must not be listed). Prints each load of a FIELD (lha/lhz/lbz/lwz
rX,FIELD(rY)) followed by a compare of rX, with the compare's next branch, marked
[EQ] when that branch tests the EQ bit alone (beq/bne: eqwhole applies; generate.py checks it again), [other]
otherwise (blt, bge...: not eligible) and [EQ0] for `== 0`: a countdown that rests at 0 (`if (t) t--`) reads 0
on every tick after it ran out, a state, not an event, so 30 takes that branch every tick too; eqwhole there
would send the half step down the running timer's branch. Rule an [EQ0] only for a count that passes 0 (one
counted down without the guard, or up from below). The loaded register is followed for 16 instructions
until something writes it, so a second compare of it (`== 1 || == 0x46`) is listed too. With --rules TAG
the [EQ] ones are printed as tick-rule lines (TAG starts their description). Session bottom's "bossfights"
(2026-10-06).
"""
import argparse
import glob
import os
import re

LOAD = re.compile(r"^([0-9a-f]{8})\s+(lha|lhz|lbz|lwz)\s+r(\d+),(-?0x[0-9a-f]+|\d+)\(r(\d+)\)")
INS = re.compile(r"^([0-9a-f]{8})\s+(\S+)\s*(.*)$")
CMP_OPS = {"cmpwi": "cmpi", "cmplwi": "cmpli", "cmpw": "cmp", "cmplw": "cmpl"}


def sites(d, fields):
    for path in sorted(glob.glob(os.path.join(d, "f_*.s"))):
        fn = os.path.basename(path)[:-2]
        lines = [l.rstrip() for l in open(path) if re.match(r"^[0-9a-f]{8}\s", l)]
        loaded = {}                                 # register -> (line, field) of its last load of a FIELD
        for n, l in enumerate(lines):
            mm = INS.match(l)
            op, args = mm.group(2), mm.group(3)
            if op in CMP_OPS:
                reg = re.sub(r"^cr\d,", "", args).split(",")[0].lstrip("r")
                if reg in loaded and n - loaded[reg][0] <= 16:
                    for q in range(n + 1, min(n + 12, len(lines))):
                        bm = INS.match(lines[q])
                        bop = bm.group(2)
                        if bop.startswith("b") and bop not in ("bl", "bla", "blrl", "bctrl"):   # the next branch, not a call
                            eq = bool(re.match(r"^(beq|bne)(lr|ctr)?[+-]?$", bop))
                            if eq and re.search(r",(0x0|0)$", args):
                                eq = "0"
                            yield fn, mm.group(1), op, args, loaded[reg][1], bm.group(1), bop, bm.group(3), eq
                            break
                continue
            m = LOAD.match(l)
            if m:
                if int(m.group(4), 16) in fields:
                    loaded[m.group(3)] = (n, m.group(4))
                else:
                    loaded.pop(m.group(3), None)
                continue
            # anything else that writes a register (its first operand; stores, compares and branches write none)
            regs = re.findall(r"\br(\d+)\b", args)
            if regs and not op.startswith(("st", "cmp", "b", "tw")):
                loaded.pop(regs[0], None)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dir")
    ap.add_argument("fields", nargs="+")
    ap.add_argument("--rules", metavar="TAG")
    a = ap.parse_args()
    fields = set(int(f, 16) for f in a.fields)
    for fn, ea, op, args, field, bea, bop, btarget, eq in sites(a.dir, fields):
        if a.rules:
            if eq is True:
                value = args.split(",")[-1]
                what = f"== {value}" if not value.startswith("r") else "== a value"
                print(f"{ea.upper()}    eqwhole     {CMP_OPS[op]:15s} {a.rules} {fn}: +{field[2:].upper()} {what}")
        else:
            mark = {True: "EQ", "0": "EQ0", False: "other"}[eq]
            print(f"{fn} {ea} {op} {args:24s} field {field} -> {bea} {bop} {btarget} [{mark}]")


if __name__ == "__main__":
    main()
