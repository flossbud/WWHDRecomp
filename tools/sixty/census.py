"""What a half-tick frame writes: the store census of src/overrides/sixty.cpp, summarised.

Usage (on the worker):
    python3 tools/sixty/census.py DUMP_DIR [--top N]

DUMP_DIR/census.txt (a 60 fps run with WWHD_STATE_DUMP=DUMP_DIR WWHD_STATE_CENSUS=1) counts the
stores made during half-tick frames into actors (process name + offset) and into the game's
.data/.bss, by the guest instruction that made them. This names their functions
(config/US_v0/functions.csv) and prints, per function, how many stores it made and where: the
state the frame advances outside the game's own tick (D21).
"""
import argparse
import collections
import os

import functions


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dir')
    ap.add_argument('--top', type=int, default=40)
    args = ap.parse_args()
    rows = []
    for line in open(os.path.join(args.dir, 'census.txt')):
        p = line.split()
        rows.append((int(p[0], 16), int(p[1]), int(p[2], 16), int(p[3])))
    of = functions.Functions()
    name = {a: of.function(a) for a in {r[0] for r in rows}}

    per_fn = collections.defaultdict(lambda: [0, collections.Counter()])
    targets = collections.Counter()
    for a, n, where, count in rows:
        target = f'actor {n - 1} +{where:#x}' if n else f'global {where:08x}'
        per_fn[name[a]][0] += count
        per_fn[name[a]][1][target] += count
        targets[f'actor {n - 1}' if n else f'global {where:08x}'] += count
    print(f'{sum(r[3] for r in rows)} stores during half ticks, from {len(per_fn)} functions\n')
    print('by function (stores; the places it wrote most):')
    for fn, (count, where) in sorted(per_fn.items(), key=lambda kv: -kv[1][0])[: args.top]:
        print(f'  {count:9d}  {fn:12s}  ' + ', '.join(f'{t} x{c}' for t, c in where.most_common(4)))
    print('\nby target (actors by process name, globals by address):')
    for t, c in targets.most_common(args.top):
        print(f'  {c:9d}  {t}')


if __name__ == '__main__':
    main()
