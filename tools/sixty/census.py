"""What a half-tick frame writes: the store census of src/overrides/sixty.cpp, summarised.

Usage (on the worker):
    python3 tools/sixty/census.py DUMP_DIR [--exe build/wwhd/wwhd-null] [--top N]

DUMP_DIR/census.txt (a 60 fps run with WWHD_STATE_DUMP=DUMP_DIR WWHD_STATE_CENSUS=1) counts the
stores made during half-tick frames into actors (process name + offset) and into the game's
.data/.bss, by the host address they were made from. This names those addresses (llvm-symbolizer:
the recompiled function f_XXXXXXXX that made the store) and prints, per function, how many stores
it made and where: the state the frame advances outside the game's own tick (D21).
"""
import argparse
import collections
import os
import subprocess

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dir')
    ap.add_argument('--exe', default=os.path.join(ROOT, 'build/wwhd/wwhd-null'))
    ap.add_argument('--top', type=int, default=40)
    args = ap.parse_args()
    rows, base = [], 0
    for line in open(os.path.join(args.dir, 'census.txt')):
        p = line.split()
        if p[0] == 'base':
            base = int(p[1], 16)
            continue
        rows.append((int(p[0], 16), int(p[1]), int(p[2], 16), int(p[3])))
    addrs = sorted({r[0] for r in rows})
    query = '\n'.join(hex(a - base - 1) for a in addrs) + '\n'
    out = subprocess.run(['llvm-symbolizer', '--obj=' + args.exe, '--functions=linkage', '--no-inlines', '--output-style=GNU'],
                         input=query, capture_output=True, text=True).stdout.split('\n')
    name = {a: (fn.removeprefix('orig_') if fn and fn != '??' else hex(a)) for a, fn in zip(addrs, out[0::2])}

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
