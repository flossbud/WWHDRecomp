"""What a process's half steps get wrong: the step-doubling trial of src/overrides/sixty.cpp, reported.

Usage (on the worker):
    python3 tools/sixty/trial.py DUMP_DIR [--names NAMES.tsv] [--target N] [--top N] [--all]

DUMP_DIR/trial.txt comes from a 30 fps run with WWHD_STATE_DUMP=DUMP_DIR WWHD_60FPS_TRIAL=n,m,...:
at every tick each listed process's execute ran as two half steps, was put back, and ran as the
game's step. Each line here is a store whose bytes after the two half steps differed from theirs
after the game's step: the instruction that made it last in the half steps (function and offset;
"game's step only" when the half steps never made it), what it wrote (a process's name and
offset, a global, or the heap), in how many ticks, the largest difference and that tick's values
(before the tick -> after the half steps / after the game's step), and what the values suggest:
  twice      the half steps moved it twice as far: a per-tick step with no time step yet
  not moved  the half steps left it, the game's step moved it
  only half  the half steps moved it, the game's step didn't
Differences of a few units in the last place of a single are rounding and are left out (--all
shows them). NAMES.tsv (tools/sixty/actor_names.py) names process numbers. The output is about the
game's code: it stays on the worker.
"""
import argparse
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import functions  # noqa: E402


def value(kind, size, v):
    if kind == 'f':
        return struct.unpack('>f', struct.pack('>I', v & 0xFFFFFFFF))[0]
    if kind == 'd':
        return struct.unpack('>d', struct.pack('>Q', v))[0]
    bits = 8 * min(size, 8)
    v &= (1 << bits) - 1
    return v - (1 << bits) if v >> (bits - 1) else v


def show(kind, x):
    return f'{x:.6g}' if kind in 'fd' else str(x)


def wrapped(kind, size, d):
    """An integer field's change modulo its width (a phase that wraps: 32400 -> -32736 is +600 in s16)."""
    if kind in 'fd':
        return d
    bits = 8 * min(size, 8)
    d &= (1 << bits) - 1
    return d - (1 << bits) if d >> (bits - 1) else d


def verdict(kind, a, b, c, size=4):
    """a before the tick, b after the half steps, c after the game's step."""
    dh, dr = wrapped(kind, size, b - a), wrapped(kind, size, c - a)
    if dr == 0:
        return 'only half'
    if dh == 0:
        return 'not moved'
    r = dh / dr
    if abs(r - 2) < 0.02:
        return 'twice'
    return f'x{r:.3g}'


def rounding(kind, b, c, half, ref):
    if kind != 'f':
        return False
    return abs((half & 0xFFFFFFFF) - (ref & 0xFFFFFFFF)) <= 16 and (b == 0) == (c == 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dir')
    ap.add_argument('--names', default='/wwhd/data/ghidra-out/actor_names.tsv')
    ap.add_argument('--target', type=int, help='only stores into processes with this name')
    ap.add_argument('--top', type=int, default=80)
    ap.add_argument('--all', action='store_true', help='rounding too')
    args = ap.parse_args()
    names = {}
    if os.path.exists(args.names):
        for line in open(args.names):
            p = line.rstrip('\n').split('\t')
            if p and p[0].isdigit() and len(p) > 1 and p[1] != '?':
                names[int(p[0])] = p[1]
    of = functions.Functions()
    ticks, rows = 0, []
    for line in open(os.path.join(args.dir, 'trial.txt')):
        if line.startswith('# ticks'):
            ticks = int(line.split()[2])
            continue
        if line.startswith('#'):
            continue
        p = line.split()
        pc, ref_only, name, where, size, kind, n, worst, tick = int(p[0], 16), int(p[1]), int(p[2]), int(p[3], 16), int(p[4]), p[5], int(p[6]), float(p[7]), int(p[8])
        init, half, ref = (int(x, 16) for x in p[9:12])
        rows.append((pc, ref_only, name, where, size, kind, n, worst, tick, init, half, ref))
    print(f'{ticks} trial executes; {len(rows)} stores differed at least once\n')
    shown = 0
    by_fn = collections.Counter()
    for pc, ref_only, name, where, size, kind, n, worst, tick, init, half, ref in sorted(rows, key=lambda r: -r[6]):
        if args.target is not None and name != args.target:
            continue
        a, b, c = (value(kind, size, v) for v in (init, half, ref))
        if not args.all and rounding(kind, b, c, half, ref):
            continue
        by_fn[of.function(pc)] += 1
        if shown >= args.top:
            continue
        shown += 1
        if name == -1:
            target = f'global {where:08x}'
        elif name == 0xFFFFFFFF - 1 or name < -1:
            target = 'heap'
        else:
            target = f'{names.get(name, name)}({name}) +{where:#x}'
        who = of.name(pc) + (' (game\'s step only)' if ref_only else '')
        print(f'{n:6d} {100 * n / max(ticks, 1):5.1f}%  {who:28s} {target:28s} {kind}{size}  '
              f'worst {show(kind, worst)} at {tick}: {show(kind, a)} -> {show(kind, b)} / {show(kind, c)}  '
              f'{verdict(kind, a, b, c, size)}')
    print('\nby function (stores that differed):')
    for fn, k in by_fn.most_common(30):
        print(f'  {k:5d}  {fn}')


if __name__ == '__main__':
    main()
