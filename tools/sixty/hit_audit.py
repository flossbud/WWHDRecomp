"""Counts added to while a hit reads true, in every converted type (the hitcounts item, session main).

Usage (on the worker, after tools/ghidra/rebuild.sh; reads disassembly, so its output stays there):
    python3 tools/sixty/hit_audit.py [PROC...]

The collision resolution runs once a tick and its hit flags stay until the next one, so a converted process's half
step and its next whole step both read the tick's hit (session bottom's hits item): a count added to on a hit
(Jalhalla's mirror-shield stun, m47E + 1 per execute while the light's hit reads true: solid in half the time)
goes twice a tick. This lists, per converted process (sixty.cpp's kConvertedByDefault, or PROC...), each call of
ChkTgHit (f_025162A4) or ChkAtHit (f_025160DC) whose result is tested, and within WINDOW instructions after it a
field loaded, + or - a constant and stored back, with no tick rule on the add or the store. Read each one: an
add on a hit that also changes state (a new action) happens once anyway; one repeated while the hit reads true
is the hazard. Disassembly from /wwhd/data/ghidra-out/asm-PROC (tools/sixty/actor_rmw.py makes it); the code
ranges of neighbouring types overlap, so a site can show under a neighbour too.
"""
import glob
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
DATA = "/wwhd/data/ghidra-out"
HIT_CALLS = {"025160dc": "ChkAtHit", "025162a4": "ChkTgHit"}
WINDOW = 48


def converted():
    src = open(os.path.join(ROOT, "src/overrides/sixty.cpp")).read()
    m = re.search(r"kConvertedByDefault =(.*?);", src, re.S)
    return sorted({int(x) for x in re.findall(r"\d+", "".join(re.findall(r'"([0-9,]*)"', m.group(1))))})


def ruled():
    out = set()
    for f in [os.path.join(ROOT, "config/US_v0/tick_rules.txt")] + sorted(glob.glob(os.path.join(ROOT, "config/US_v0/tick_rules/*.txt"))):
        for line in open(f):
            m = re.match(r"([0-9A-Fa-f]{8})\s", line)
            if m:
                out.add(int(m.group(1), 16))
    return out


def main():
    procs = [int(x) for x in sys.argv[1:]] or converted()
    rules = ruled()
    seen = set()
    for p in procs:
        d = os.path.join(DATA, f"asm-{p}")
        if not os.path.isdir(d):
            subprocess.run(["uv", "run", os.path.join(HERE, "actor_rmw.py"), str(p)], capture_output=True)
        if not os.path.isdir(d):
            continue
        out = []
        for path in sorted(glob.glob(os.path.join(d, "f_*.s"))):
            fn = os.path.basename(path)[:-2]
            lines = [(int(m.group(1), 16), m.group(2).strip()) for m in
                     (re.match(r"([0-9a-f]{8})\s+(.*)", l) for l in open(path)) if m]
            for i, (a, ins) in enumerate(lines):
                call = re.match(r"bl 0x([0-9a-f]{8})", ins)
                if not call or call.group(1) not in HIT_CALLS:
                    continue
                for j in range(i + 1, min(i + WINDOW, len(lines))):
                    b, ins2 = lines[j]
                    ld = re.match(r"(lbz|lhz|lha|lwz)\s+(r\d+),(0x[0-9a-f]+)\((r\d+)\)", ins2)
                    if not ld:
                        continue
                    reg, off = ld.group(2), ld.group(3)
                    for k in range(j + 1, min(j + 5, len(lines))):
                        c, ins3 = lines[k]
                        add = re.match(rf"(addi|subi)\s+(r\d+),{reg},(-?0x[0-9a-f]+)", ins3)
                        if not add:
                            continue
                        dst = add.group(2)
                        for m_ in range(k + 1, min(k + 5, len(lines))):
                            e, ins4 = lines[m_]
                            st = re.match(rf"st[bhw]\s+{dst},{off}\(", ins4)
                            if st and c not in rules and e not in rules and e not in seen:
                                seen.add(e)
                                out.append(f"  {fn} {HIT_CALLS[call.group(1)]} at {a:08X}: field {off} {add.group(1)} "
                                           f"{add.group(3)} at {c:08X}, stored {e:08X}")
                                break
                        break
        if out:
            print(f"== {p}")
            print("\n".join(out))


if __name__ == "__main__":
    main()
