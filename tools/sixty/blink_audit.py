"""Audit converted processes for blinks that still step twice a tick at 60 fps (the npcs item, session top).
Usage (on the worker, after tools/ghidra/rebuild.sh; reads the disassembly dumps):
    python3 tools/sixty/blink_audit.py [-a]
NPCs blink with a countdown in one of the shared helpers cLib_calcTimer<s16> (f_02055B64) or <int>
(f_0211D2F8): once it is out, the blink's frame + 1 a step until its length, then a new wait. Both helpers
are `keep` in tick_rules.txt (a half step returns the count as it is), so on a half step an out countdown
still says 0 and the frame steps twice a tick. The trial only sees the blinks its test reaches: many NPCs
blink in a mode (talking, an event) a walk past never starts. This lists, per process converted by default
(-a: every process), each call of a helper followed by a compare of r3 and, within 30 instructions, a field
+ 1 stored back, with no rule on the call, the add or the store. The fix used: `keep:REG` on the frame's add
(Outset's NPCs; covers a mode with no countdown too), or `whole:r3=1` on the call plus `whole` on the
store (Windfall's houses). Not every hit is a blink: a state's number + 1 once a countdown is out moves on to
other code (at most half a tick early), and a random wait's `rand & 1` + 1 is no count; read each one.
Code ranges and dumps as tools/sixty/counter_audit.py's. The output names the game's instructions: keep it
on the worker.
"""
import argparse
import csv
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import counter_audit as ca  # noqa: E402

HELPERS = ("0x02055b64", "0x0211d2f8")


def sites(path):
    """(call, store, field, add) of each helper call whose result is compared and a field + 1 follows."""
    lines = [l.split(None, 2) for l in open(path) if re.match(r"[0-9a-f]{8}  ", l)]
    for i, l in enumerate(lines):
        if len(l) < 3 or l[1] != "bl" or not l[2].startswith(HELPERS):
            continue
        if not any(len(x) == 3 and x[1] in ("cmpwi", "cmpw") and x[2].startswith("r3,") for x in lines[i + 1:i + 4]):
            continue
        for j in range(i + 1, min(len(lines), i + 30)):
            x = lines[j]
            m = re.match(r"addi (r\d+),(r\d+),0x1$", x[1] + " " + x[2].strip()) if len(x) == 3 else None
            if not m:
                continue
            st = next((y for y in lines[j + 1:j + 6] if len(y) == 3 and y[1] in ("stb", "sth", "stw")
                       and y[2].startswith(m.group(1) + ",")), None)
            if st:
                yield int(l[0], 16), int(st[0], 16), st[2].split(",", 1)[1].strip(), int(x[0], 16)
                break


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-a", action="store_true", help="every process, not only those converted by default")
    args = ap.parse_args()
    rules, conv = ca.tick_rules(), ca.converted()
    names, profiles = {}, {}
    for line in open(os.path.join(ca.DATA, "actor_names.tsv")):
        p = line.rstrip("\n").split("\t")
        if p and p[0].isdigit():
            names[int(p[0])] = p[1]
    for line in open(os.path.join(ca.DATA, "actor_profiles.tsv")):
        p = line.rstrip("\n").split("\t")
        if len(p) > 7 and p[1].isdigit():
            methods = [int(x, 16) for x in p[7:] if re.fullmatch(r"[0-9A-Fa-f]{8}", x) and int(x, 16) > 0x02000000]
            if methods:
                profiles[int(p[1])] = methods
    ends = sorted((max(ms), pr) for pr, ms in profiles.items())
    functions = sorted(int(r[0], 16) for r in csv.reader(open(os.path.join(ca.ROOT, "config/US_v0/functions.csv")))
                       if r and re.fullmatch(r"[0-9A-Fa-f]{8}", r[0]))
    dumps = {}
    for path in glob.glob(os.path.join(ca.DATA, "asm-*/f_*.s")):
        dumps.setdefault(int(os.path.basename(path)[2:10], 16), path)
    for proc in sorted(profiles if args.a else conv):
        if proc not in profiles:
            continue
        hi = max(profiles[proc])
        before = [e for e, pr in ends if e < min(profiles[proc]) and pr != proc]
        lo = max(before[-1] if before else hi - 0x8000, min(profiles[proc]) - 0x20000)
        for a in functions:
            if lo < a <= hi and a in dumps:
                for call, store, field, add in sites(dumps[a]):
                    if not rules.get(call) and not rules.get(add) and not rules.get(store):
                        print(f"{proc:3d} {names.get(proc, '?'):14s}{'' if proc in conv else ' [not converted]'}"
                              f"  f_{a:08X}: call {call:08X}, add {add:08X}, store {store:08X} {field}")


if __name__ == "__main__":
    main()
