"""An actor type's per-tick candidates, for converting it to 60 fps (D21).

Usage (on the worker, after tools/ghidra/rebuild.sh):
    uv run tools/sixty/actor_rmw.py PROCESS [--out DIR]

PROCESS is a process name (number) from /wwhd/data/ghidra-out/actor_profiles.tsv. Its code is taken
as the functions around its profile's methods, up to the nearest functions of other source files
(/wwhd/data/ghidra-out/source_files.tsv: the assert strings) or a gap of 0x2000 bytes with no
function. Each is disassembled (tools/ghidra/disasm.py, into DIR, default
/wwhd/data/ghidra-out/asm-PROCESS) and read by tools/sixty/rmw.py: every read-modify-write store,
with a suggested tick rule:
  whole   an integer field + or - a constant (a counter or a timer; `late` if the code reads it
          before adding to it within the tick, see D21)
  *h@REG  a float field + another value (a per-tick amount; REG the amount's register)
  k@REG   a float field approached (x + (t - x) k; REG the factor's register)
  ?       anything else: read the chain
The suggestions are a start: the step-doubling trial (WWHD_60FPS_TRIAL) and a 30-against-60 track
(tools/sixty/compare.py --track) decide. The output names the game's instructions: keep it on the
worker.
"""
import argparse
import bisect
import csv
import os
import re
import subprocess
import sys

DATA = "/wwhd/data/ghidra-out"
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))


def functions():
    out = []
    with open(os.path.join(ROOT, "config/US_v0/functions.csv")) as f:
        for row in csv.reader(f):
            if row and re.fullmatch(r"[0-9A-Fa-f]{8}", row[0]):
                out.append((int(row[0], 16), int(row[1], 0) if row[1] else 0))
    return sorted(out)


def code_range(process):
    methods = None
    with open(os.path.join(DATA, "actor_profiles.tsv")) as f:
        for line in f:
            p = line.rstrip("\n").split("\t")
            if len(p) > 7 and p[1] == str(process):
                methods = [int(x, 16) for x in p[7:] if re.fullmatch(r"[0-9A-Fa-f]{8}", x)]
                break
    if not methods:
        sys.exit(f"process {process}: no profile in actor_profiles.tsv")
    files = []
    with open(os.path.join(DATA, "source_files.tsv")) as f:
        for line in f:
            p = line.split("\t")
            if len(p) > 1 and re.fullmatch(r"[0-9A-Fa-f]{8}", p[1]):
                files.append((int(p[1], 16), p[0]))
    files.sort()
    lo, hi = min(methods), max(methods)
    # the file the methods' neighbourhood names most
    near = [name for a, name in files if lo - 0x4000 <= a <= hi + 0x4000]
    own = max(set(near), key=near.count) if near else None
    before = [a for a, name in files if a < lo and name != own]
    after = [a for a, name in files if a > hi and name != own]
    return (before[-1] if before else lo - 0x10000), (after[0] if after else hi + 0x10000), own, methods


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("process", type=int)
    ap.add_argument("--out")
    args = ap.parse_args()
    lo, hi, own, methods = code_range(args.process)
    fns = functions()
    starts = [a for a, _ in fns]
    i = bisect.bisect_right(starts, lo)
    picked = []
    last_end = None
    for a, size in fns[i:]:
        if a >= hi:
            break
        if last_end is not None and a - last_end > 0x2000 and a > max(methods):
            break
        picked.append(a)
        last_end = a + size
    out = args.out or os.path.join(DATA, f"asm-{args.process}")
    os.makedirs(out, exist_ok=True)
    print(f"# process {args.process} ({own}): {len(picked)} functions {picked[0]:08X}-{last_end:08X}")
    todo = [a for a in picked if not os.path.exists(os.path.join(out, f"f_{a:08X}.s"))]
    for k in range(0, len(todo), 40):
        subprocess.run(["uv", "run", os.path.join(ROOT, "tools/ghidra/disasm.py")] + [f"{a:08X}" for a in todo[k:k + 40]]
                       + ["--out", out], cwd=ROOT, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    asm = [os.path.join(out, f"f_{a:08X}.s") for a in picked if os.path.exists(os.path.join(out, f"f_{a:08X}.s"))]
    r = subprocess.run([sys.executable, os.path.join(HERE, "rmw.py")] + asm, capture_output=True, text=True)
    # rmw.py: a site line (address, store), then its chain indented
    site = None
    chain = []

    def flush():
        if not site:
            return
        ea, insn = site
        ops = [c.split(None, 1)[1] if len(c.split(None, 1)) > 1 else "" for c in chain]
        rule = "?"
        if insn.startswith(("stw", "sth", "stb")):
            if any(o.startswith(("addi ", "subi ", "addic ")) for o in ops):
                rule = "whole"
        elif insn.startswith("stfs"):
            fm = [o for o in ops if o.startswith(("fmadds", "fmsubs", "fnmsubs"))]
            fa = [o for o in ops if o.startswith(("fadds", "fsubs"))]
            if fm:
                regs = re.findall(r"f\d+", fm[-1])
                rule = f"k@{regs[2]}" if len(regs) > 2 else "?"
            elif fa:
                regs = re.findall(r"f\d+", fa[-1])
                rule = f"*h@{regs[2]}" if len(regs) > 2 else "?"
        print(f"{ea.upper():10s}  {rule:10s}  {insn:28s}  # " + " | ".join(ops[-3:]))

    for line in r.stdout.splitlines():
        if line.startswith("//") or not line.strip():
            continue
        if not line.startswith(" "):
            flush()
            p = line.split(None, 1)
            site = (p[0], p[1] if len(p) > 1 else "")
            chain = []
        else:
            chain.append(line.strip())
    flush()


if __name__ == "__main__":
    main()
