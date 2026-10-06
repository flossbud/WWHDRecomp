"""Equality tests of whole-tick counts in every converted type (the eqwhole item, session main).

Usage (on the worker, after tools/ghidra/rebuild.sh; reads disassembly, so its output stays there):
    python3 tools/sixty/eq_audit.py [PROC...]

A count a rule keeps to whole ticks (`whole` on its store, `keep` on its add) reads the same on a stepping half step
as on the whole step before it, so `if (count == N)` acts twice a tick at 60 (Gohma's armor cracked after two
rocks). For each converted process (sixty.cpp's kConvertedByDefault, or PROC...): its code's disassembly
(/wwhd/data/ghidra-out/asm-PROC, made by tools/sixty/actor_rmw.py if missing), the fields its whole/keep rules
store to (the rule's store, or for keep the add's register's next store), then tools/sixty/eqsites.py on them:
loads of those fields compared and branched on EQ, with no eqwhole rule yet. Read each one: a second compare of
the same register isn't seen (eqsites.py's caveat), and some EQ tests are meant per step.
"""
import glob
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
DATA = "/wwhd/data/ghidra-out"
# the bosses: session bottom rules theirs as it fights them (tick_rules/bosses.txt, the bossfights item)
BOSSES = {234, 235, 236, 237, 240, 211, 217, 218, 219, 238, 243, 244, 245, 246, 225, 228}


def converted():
    src = open(os.path.join(ROOT, "src/overrides/sixty.cpp")).read()
    m = re.search(r"kConvertedByDefault =(.*?);", src, re.S)
    return sorted({int(x) for x in re.findall(r"\d+", "".join(re.findall(r'"([0-9,]*)"', m.group(1))))})


def rules():
    out = {}
    for f in [os.path.join(ROOT, "config/US_v0/tick_rules.txt")] + sorted(glob.glob(os.path.join(ROOT, "config/US_v0/tick_rules/*.txt"))):
        for line in open(f):
            m = re.match(r"([0-9A-Fa-f]{8})\s+(\S+)", line)
            if m:
                out[int(m.group(1), 16)] = m.group(2)
    return out


def asm_dir(p):
    d = os.path.join(DATA, f"asm-{p}")
    if not os.path.isdir(d):
        subprocess.run(["uv", "run", os.path.join(HERE, "actor_rmw.py"), str(p)], capture_output=True)
    return d if os.path.isdir(d) else None


def lines_of(d):
    out = []
    for f in sorted(glob.glob(os.path.join(d, "*.s"))):
        for line in open(f):
            m = re.match(r"([0-9a-f]{8})\s+(.*)", line)
            if m:
                out.append((int(m.group(1), 16), m.group(2).strip()))
    return out


SITES = {}


def main():
    procs = [int(x) for x in sys.argv[1:]] or [p for p in converted() if p not in BOSSES]
    rs = rules()
    for p in procs:
        d = asm_dir(p)
        if not d:
            print(f"{p}: no disassembly"); continue
        code = lines_of(d)
        at = {a: i for i, (a, _) in enumerate(code)}
        fields = set()
        near = {}                                   # field -> the addresses of the rules that keep it
        for a, kind in rs.items():
            if a not in at or not (kind == "whole" or kind.startswith("keep")):
                continue
            i = at[a]
            ins = code[i][1]
            st = re.match(r"st[bhw]\w*\s+r\d+,(-?0x[0-9a-f]+|\d+)\(r\d+\)", ins)
            if st:
                f = int(st.group(1), 16) if st.group(1).startswith(("0x", "-0x")) else int(st.group(1))
                fields.add(f); near.setdefault(f, []).append(a)
                continue
            ad = re.match(r"(addi|subi|add)\s+(r\d+),", ins)
            if ad and kind.startswith("keep"):
                reg = ad.group(2)
                for _, nxt in code[i + 1:i + 12]:
                    st = re.match(rf"st[bhw]\w*\s+{reg},(-?0x[0-9a-f]+|\d+)\(r\d+\)", nxt)
                    if st:
                        f = int(st.group(1), 16)
                        fields.add(f); near.setdefault(f, []).append(a); break
        fields = {f for f in fields if f > 0x100}           # the actor's own fields, not the stack's
        if not fields:
            continue
        r = subprocess.run([sys.executable, os.path.join(HERE, "eqsites.py"), d] + [hex(f) for f in sorted(fields)],
                           capture_output=True, text=True)
        hits = [l for l in r.stdout.splitlines() if "[EQ]" in l]
        hits = [l for l in hits if not any(rs.get(int(x, 16)) == "eqwhole" for x in re.findall(r"\b([0-9a-f]{8})\b", l))]
        keep = []
        for l in hits:
            m = re.search(r"([0-9a-f]{8})\s+(cmp\w*)\s+r\d+,(\S+)\s+field (0x[0-9a-f]+)", l)
            if not m:
                continue
            site, op, arg, f = int(m.group(1), 16), m.group(2), m.group(3), int(m.group(4), 16)
            if op.startswith("cmp") and arg in ("-0x1",):            # a sentinel it rests at: a state, as == 0
                continue
            if not any(abs(site - a) < 0x4000 for a in near.get(f, [])):   # the field's rule in another type's code
                continue
            keep.append((site, l))
        hits = [l for _, l in keep]
        for site, l in keep:
            SITES.setdefault(site, (p, l))
        if hits:
            print(f"== {p}: fields {', '.join(hex(f) for f in sorted(fields))}")
            for l in hits:
                print("  " + l)
    print("== rule lines (eqwhole on each kept site, once):")
    for site, (p, l) in sorted(SITES.items()):
        op = re.search(r"\s(cmp\w*)\s", l).group(1)
        print(f"{site:08X}    eqwhole     {op:15s} {p}: {l.split('field')[1].split('->')[0].strip()} == {l.split(',')[1].split()[0]}")


if __name__ == "__main__":
    main()
