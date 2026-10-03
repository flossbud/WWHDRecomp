"""Which actor types run right converted: a 30-against-60 track of every actor, ranked by type.

Usage (on the worker):
    WWHD_STATE_TRACK=all WWHD_60FPS_CONVERT=all tools/sixty/run.sh ROUTE OUT 30 60
    python3 tools/sixty/actor_types.py OUT/30 OUT/60 [--from TICK] [--names NAMES.tsv]

A converted process reaches a tick's state at that tick's half frame, so tick k of the 30-tick run
is compared with the half frame after tick k of the 60 run (D21). Per process type: its instances,
the ticks compared, how many matched byte for byte, and the fields that differed most (offset:
ticks). The sound source's listener-relative fields (fopAc +0x110-0x127, +0x198-0x19F,
+0x1D0-0x1DB, written from the camera's position) are left out: they follow the camera, which isn't
an actor. A type whose instances match at every tick needs no rules of its own; the rest are
where tools/sixty/actor_rmw.py and the step-doubling trial come in. The dumps are game memory:
keep them on the worker.
"""
import argparse
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import compare  # noqa: E402

IGNORED = [(0x110, 0x128), (0x198, 0x1A0), (0x1D0, 0x1DC)]


def ignored(o):
    return any(a <= o < b for a, b in IGNORED)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--from", dest="first", type=int, default=0)
    ap.add_argument("--names")
    args = ap.parse_args()
    names = {}
    if args.names:
        for line in open(args.names):
            p = line.rstrip("\n").split("\t")
            if p and p[0].isdigit():
                names[int(p[0])] = p[1]
    A = compare.load_track(os.path.join(args.a, "track.bin"))
    B = compare.load_track(os.path.join(args.b, "track.bin"))
    rows = collections.defaultdict(lambda: [0, 0, 0, collections.Counter()])
    for key, ticks in A.items():
        if key not in B:
            continue
        r = rows[key[0]]
        r[0] += 1
        for t2, a in ticks.items():
            if t2 % 2 or t2 // 2 < args.first:
                continue
            b = B[key].get(t2 + 1)
            if b is None:
                continue
            r[1] += 1
            offs = [o for o in range(0, min(len(a), len(b)), 4) if a[o:o + 4] != b[o:o + 4] and not ignored(o)]
            if not offs:
                r[2] += 1
            for o in offs:
                r[3][o] += 1
    print(f"{'type':>5} {'name':14} {'inst':>4} {'ticks':>6} {'same':>6}  fields differing most (offset:ticks)")
    for name, (inst, n, same, offs) in sorted(rows.items(), key=lambda kv: (kv[1][2] / max(1, kv[1][1]), kv[0])):
        top = " ".join(f"{o:x}:{c}" for o, c in offs.most_common(6))
        print(f"{name:5d} {names.get(name, '?')[:14]:14} {inst:4d} {n:6d} {100 * same / max(1, n):5.1f}%  {top}")


if __name__ == "__main__":
    main()
