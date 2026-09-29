# /// script
# requires-python = ">=3.10"
# dependencies = ["zstandard", "numpy"]
# ///
"""G0 (docs/recompiler-design.md D15): what WWHD asks of GX2 along a route, from an HLE trace.

Usage:
    uv run tools/reference/g0_gx2.py TRACE [--prefix gx2.] [--imports build/recomp/imports.cpp]

For every traced function whose name starts with PREFIX: the number of calls, the first and last
frame, the threads that call it, and for each argument register r3..r10 and f1..f8 the values it
takes (the most common few, or how many distinct ones if there are many). With --imports (the
generator's import table), the imported functions of that library that are never called are
listed too. The trace is streamed (hle_trace.blocks), so memory stays bounded.
"""
import argparse
import collections
import pathlib
import re
import sys

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from hle_trace import blocks  # noqa: E402

CAP = 256  # distinct values tracked per register; beyond that only "many" is reported


class Stats:
    def __init__(self):
        self.calls = 0
        self.first = self.last = None
        self.threads = collections.Counter()
        self.gpr = [collections.Counter() for _ in range(8)]
        self.fpr = [collections.Counter() for _ in range(8)]
        self.gpr_many = [False] * 8
        self.fpr_many = [False] * 8

    def add(self, calls):
        self.calls += len(calls)
        f = calls["frame"]
        self.first = int(f.min()) if self.first is None else min(self.first, int(f.min()))
        self.last = int(f.max()) if self.last is None else max(self.last, int(f.max()))
        for t, n in zip(*np.unique(calls["thread"], return_counts=True)):
            self.threads[int(t)] += int(n)
        for r in range(8):
            for col, counters, many in ((calls["gpr"][:, r], self.gpr, self.gpr_many),
                                        (calls["fpr"][:, r], self.fpr, self.fpr_many)):
                if many[r]:
                    continue
                vals, counts = np.unique(col, return_counts=True)
                c = counters[r]
                for v, n in zip(vals.tolist(), counts.tolist()):
                    c[v] += n
                if len(c) > CAP:
                    many[r] = True
                    c.clear()


def show_values(counter, many, is_float):
    if many:
        return f"many (>{CAP} distinct)"
    if not counter:
        return ""
    if len(counter) == 1 and not next(iter(counter)):
        return None                                    # always 0: not worth printing
    fmt = (lambda v: f"{v:g}") if is_float else (lambda v: f"{v:#x}")
    top = counter.most_common(6)
    s = ", ".join(f"{fmt(v)}×{n}" for v, n in top)
    return s + (f", ... ({len(counter)} distinct)" if len(counter) > len(top) else "")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("trace")
    ap.add_argument("--prefix", default="gx2.")
    ap.add_argument("--imports", help="the generator's imports.cpp, to list imported functions never called")
    args = ap.parse_args()

    stats = collections.defaultdict(Stats)
    names = {}
    frames = 0
    total = 0
    for names, block in blocks(args.trace):
        total += len(block)
        frames = max(frames, int(block["frame"].max()))
        wanted = [i for i, n in names.items() if n.startswith(args.prefix)]
        if not wanted:
            continue
        sel = block[np.isin(block["index"], wanted)]
        if not len(sel):
            continue
        order = np.argsort(sel["index"], kind="stable")
        sel = sel[order]
        idx, starts = np.unique(sel["index"], return_index=True)
        bounds = list(starts[1:]) + [len(sel)]
        for i, a, b in zip(idx.tolist(), starts.tolist(), bounds):
            stats[names[i]].add(sel[a:b])
        print(f"... {total} calls read", file=sys.stderr, flush=True)   # heartbeat

    called = {n for n in stats}
    print(f"{args.trace}: {total} calls over {frames} frames; {len(called)} functions matching '{args.prefix}' called")
    for name, s in sorted(stats.items(), key=lambda kv: -kv[1].calls):
        print(f"\n{name}: {s.calls} calls, frames {s.first}-{s.last}, "
              f"threads {', '.join(f'{t:08x}×{n}' for t, n in s.threads.most_common(3))}")
        for r in range(8):
            g = show_values(s.gpr[r], s.gpr_many[r], False)
            if g:
                print(f"  r{r + 3}: {g}")
        for r in range(8):
            f = show_values(s.fpr[r], s.fpr_many[r], True)
            if f:
                print(f"  f{r + 1}: {f}")
    if args.imports:
        lib = args.prefix.rstrip(".")
        imported = set()
        for m in re.finditer(r'\{0x[0-9A-F]+u, "([^"]+)", "([^"]+)", 0u\}', pathlib.Path(args.imports).read_text()):
            if m.group(1).split(".")[0] == lib:
                imported.add(f"{lib}.{m.group(2)}")
        never = sorted(imported - called)
        print(f"\nimported from {lib}: {len(imported)}; called on this route: {len(imported & called)}; never called ({len(never)}):")
        for n in never:
            print(f"  {n}")


if __name__ == "__main__":
    main()
