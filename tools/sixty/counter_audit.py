"""Audit converted processes for counters that still count on both frames of a tick at 60 fps (D21).

Usage (on the worker, after tools/ghidra/rebuild.sh; reads the disassembly dumps):
    python3 tools/sixty/counter_audit.py [PROCESS...] [-v] [--up]

A process converted to 60 fps executes on half ticks too, so every `timer--` and `count++` in its code
needs a tick rule (config/US_v0/tick_rules.txt: `whole`, `late` or `keep`), or it runs at twice the
speed. The step-doubling trial (WWHD_60FPS_TRIAL) only sees the counters that run in its test: a
Moblin's "can't block for 25 ticks after a hit" is zero until it is hit (bug B14), Link's "can't be
hit again for 30 ticks" until he is. This lists, per process, every stored counter (a field loaded,
+ or - 1, stored back to the same field within a few instructions) whose store and add have no rule.

A process's code is taken as the functions after the previous actor profile's last method up to its
own last method (a source file's static functions come before its methods), from
/wwhd/data/ghidra-out/actor_profiles.tsv; functions with no dump in /wwhd/data/ghidra-out/asm-*/ are
counted as "not dumped" (tools/sixty/actor_rmw.py PROCESS or tools/ghidra/disasm.py dump them). With
no PROCESS: every process converted by default (src/overrides/sixty.cpp), those with a countdown
without a rule only. -v lists the sites (store address, +-1, field, function); --up also counts the
+ 1 ones in the summary's filter (many of those are a state's number, not a time).

Then read each site in the decomp: tested before the - 1 (`if (n > 0) n--; else next();`) wants
`late` on the store, tested after it (`n--; if (n == 0) ...`) `keep:REG` on the add, otherwise
`whole` on the store. Input-driven counts (a ReDead's break-free count) and state numbers want none.
The output names the game's instructions: keep it on the worker.
"""
import argparse
import csv
import glob
import os
import re

DATA = "/wwhd/data/ghidra-out"
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))


def tick_rules():
    rules = {}
    for path in [os.path.join(ROOT, "config/US_v0/tick_rules.txt")] + glob.glob(os.path.join(ROOT, "config/US_v0/tick_rules/*.txt")):
        for line in open(path):
            m = re.match(r"([0-9A-Fa-f]{8})\s+(\S+)", line)
            if m:
                rules[int(m.group(1), 16)] = m.group(2)
    return rules


def converted():
    src = open(os.path.join(ROOT, "src/overrides/sixty.cpp")).read()
    k = src.index("kConvertedByDefault")
    return set(int(x) for x in re.findall(r"\d+", " ".join(re.findall(r'"([0-9, ]+)"', src[k:k + 12000]))))


def counters(path):
    """(store address, sign, field, add address) of every field loaded, +-1 and stored back in a dump."""
    lines = [l.split(None, 2) for l in open(path) if re.match(r"[0-9a-f]{8}  ", l)]
    for i, l in enumerate(lines):
        if len(l) < 3:
            continue
        m = re.match(r"(subi|addi) (r\d+),(r\d+),(-?0x1)$", l[1] + " " + l[2])
        if not m:
            continue
        dst, src = m.group(2), m.group(3)
        store = next((x for x in lines[i + 1:i + 7] if len(x) == 3 and x[1] in ("sth", "stb", "stw", "sthx", "stbx", "stwx")
                      and x[2].startswith(dst + ",") and "(r1)" not in x[2]), None)
        if not store:
            continue
        field = store[2].split(",", 1)[1]             # D(rA), or rA,rB of an indexed store (a loop over timers)
        if not any(len(x) == 3 and x[1] in ("lha", "lhz", "lbz", "lwz", "lhax", "lhzx", "lbzx", "lwzx") and x[2] == src + "," + field
                   for x in lines[max(0, i - 14):i]):
            continue
        down = (m.group(1) == "subi") != m.group(4).startswith("-")
        yield int(store[0], 16), "-" if down else "+", field, int(l[0], 16)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("process", nargs="*", type=int)
    ap.add_argument("-v", action="store_true", help="list the sites")
    ap.add_argument("--up", action="store_true", help="count the + 1 sites too")
    args = ap.parse_args()
    rules, conv = tick_rules(), converted()
    names, profiles = {}, {}
    for line in open(os.path.join(DATA, "actor_names.tsv")):
        p = line.rstrip("\n").split("\t")
        if p and p[0].isdigit():
            names[int(p[0])] = p[1]
    for line in open(os.path.join(DATA, "actor_profiles.tsv")):
        p = line.rstrip("\n").split("\t")
        if len(p) > 7 and p[1].isdigit():
            methods = [int(x, 16) for x in p[7:] if re.fullmatch(r"[0-9A-Fa-f]{8}", x) and int(x, 16) > 0x02000000]
            if methods:
                profiles[int(p[1])] = methods
    ends = sorted((max(ms), pr) for pr, ms in profiles.items())
    functions = []
    for row in csv.reader(open(os.path.join(ROOT, "config/US_v0/functions.csv"))):
        if row and re.fullmatch(r"[0-9A-Fa-f]{8}", row[0]):
            functions.append(int(row[0], 16))
    functions.sort()
    dumps = {}
    for path in glob.glob(os.path.join(DATA, "asm-*/f_*.s")):
        dumps.setdefault(int(os.path.basename(path)[2:10], 16), path)
    for proc in args.process or sorted(conv):
        if proc not in profiles:
            continue
        hi = max(profiles[proc])
        before = [e for e, pr in ends if e < min(profiles[proc]) and pr != proc]
        lo = max(before[-1] if before else hi - 0x8000, min(profiles[proc]) - 0x20000)
        picked = [a for a in functions if lo < a <= hi]
        sites = [(s, sign, field, a) for a in picked if a in dumps for s, sign, field, add in counters(dumps[a])
                 if not rules.get(s) and not rules.get(add)]
        shown = [x for x in sites if args.up or x[1] == "-"]
        if not shown and not args.process:
            continue
        print(f"{proc:3d} {names.get(proc, '?'):14s} {lo:08X}-{hi:08X} {len(picked):3d} functions ({sum(a not in dumps for a in picked)} not dumped):"
              f" without a rule {sum(x[1] == '-' for x in sites)} countdowns, {sum(x[1] == '+' for x in sites)} counts up"
              f"{'' if proc in conv else '  [not converted]'}")
        if args.v:
            for s, sign, field, a in sites:
                print(f"      {s:08X} {sign}1 {field:14s} f_{a:08X}")


if __name__ == "__main__":
    main()
