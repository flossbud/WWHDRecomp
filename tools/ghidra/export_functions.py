# /// script
# requires-python = ">=3.10"
# dependencies = ["pyghidra==3.0.2"]
# ///
"""Export every function Ghidra knows about to config/US_v0/functions.csv.

Usage (after tools/ghidra/import.sh):
    uv run tools/ghidra/export_functions.py [--project ghidra/projects] [--out config/US_v0/functions.csv]

Columns:
    address     entry point, 8 hex digits
    size        bytes in the function body (all ranges)
    end         one past the highest body address
    ranges      number of address ranges in the body (>1 = non-contiguous)
    name        current Ghidra name (FUN_xxxxxxxx when unnamed)
    source      DEFAULT | ANALYSIS | IMPORTED | USER_DEFINED
    thunk       1 if Ghidra marked it as a thunk
The file is sorted by address so diffs between analysis runs stay readable.
"""
import argparse
import csv
import sys
from pathlib import Path

import pyghidra

import wwhd_env

ROOT = wwhd_env.ROOT


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--project", default=wwhd_env.PROJECT_DIR)
    ap.add_argument("--program", default=wwhd_env.PROGRAM)
    ap.add_argument("--out", default=ROOT / "config" / "US_v0" / "functions.csv")
    args = ap.parse_args()

    wwhd_env.start()
    project = wwhd_env.open_project(args.project)
    try:
        with pyghidra.program_context(project, args.program) as program:
            rows = list(collect(program))
            text = program.getMemory().getBlock(".text")
            lo, hi = text.getStart().getOffset(), text.getEnd().getOffset()
            text_size = text.getSize()
    finally:
        project.close()

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["address", "size", "end", "ranges", "name", "source", "thunk"])
        w.writerows(rows)

    covered = sum(r[1] for r in rows if lo <= int(r[0], 16) <= hi)
    named = sum(1 for r in rows if r[5] != "DEFAULT")
    print(f"{len(rows)} functions -> {out.relative_to(ROOT) if out.is_relative_to(ROOT) else out}")
    print(f"  .text covered: {covered:#x} / {text_size:#x} bytes ({100 * covered / max(text_size, 1):.1f}%)")
    print(f"  named (non-DEFAULT): {named}")
    print(f"  non-contiguous bodies: {sum(1 for r in rows if r[3] > 1)}")


def collect(program):
    fm = program.getFunctionManager()
    for fn in fm.getFunctions(True):
        if fn.isExternal():
            continue
        body = fn.getBody()
        entry = fn.getEntryPoint().getOffset()
        end = body.getMaxAddress().getOffset() + 1 if not body.isEmpty() else entry
        yield [
            f"{entry:08X}",
            int(body.getNumAddresses()),
            f"{end:08X}",
            int(body.getNumAddressRanges()),
            fn.getName(True),
            str(fn.getSymbol().getSource()),
            int(fn.isThunk()),
        ]


if __name__ == "__main__":
    sys.exit(main())
