"""Call paths through a function, from a wwhd-null CPU profile (WWHD_PROFILE=path).

Usage (on the worker):
    python3 tools/profile_tree.py PROFILE FUNC [--up K] [--down DEPTH] [--min PCT]

Recompiled guest functions are host functions (f_XXXXXXXX), so the profiler's host stacks are the
guest's call chains. For FUNC (f_XXXXXXXX or a name from config/US_v0/symbols.csv):
  --up K       the K most sampled call paths that reach FUNC (guest frames, outermost first)
  --down DEPTH the call tree below FUNC to DEPTH levels: each callee's share of FUNC's samples
               (inclusive), children under their caller
  --min PCT    hide tree nodes under PCT percent of FUNC's samples (default 1)
Only guest frames (f_XXXXXXXX) and the runtime's entry are kept in paths; OS calls show as their
host function's name.
"""
import collections
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location('profile_report', os.path.join(HERE, 'profile_report.py'))
report = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(report)


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        sys.exit(__doc__)
    path, func = args[0], args[1]

    def opt(name, default):
        return type(default)(args[args.index(name) + 1]) if name in args else default
    up, down, min_pct = opt('--up', 10), opt('--down', 3), opt('--min', 1.0)

    info, threads, segments, samples = report.load(path)
    names = report.symbolize(info, segments, {a for _, st in samples for a in st})
    gnames = report.guest_names()
    by_name = {v: k for k, v in gnames.items()}
    target = by_name.get(func, func)

    def short(a):
        fn = names.get(a, '?')
        fn = fn.split('(')[0]
        return fn

    def label(fn):
        return f'{fn} ({gnames[fn]})' if fn in gnames else fn

    def keep(fn):
        return fn.startswith('f_') or fn.startswith('orig_f_') or not (fn.startswith('?') or fn.startswith('wwhd_fiber')
                                                                      or 'FiberThreadEntry' in fn or fn == 'clone')

    total = 0
    paths = collections.Counter()
    tree = collections.Counter()      # tuple of frames below the target (outermost first) -> samples
    for _, stack in samples:
        # stack: innermost first
        frames = [short(a) for a in stack]
        frames = [f.removeprefix('orig_') for f in frames]
        if target not in frames:
            continue
        total += 1
        i = len(frames) - 1 - frames[::-1].index(target)      # the outermost occurrence
        above = [f for f in reversed(frames[i + 1:]) if keep(f)]
        paths[tuple(above[-12:])] += 1
        below = [f for f in reversed(frames[:i]) if keep(f)]
        for d in range(1, min(down, len(below)) + 1):
            tree[tuple(below[:d])] += 1
    if not total:
        sys.exit(f'{target}: in no sampled stack')
    print(f'{label(target)}: in {total} of {len(samples)} samples ({100 * total / len(samples):.1f}%)\n')
    print(f'top {up} call paths into it (outermost first):')
    for p, n in paths.most_common(up):
        print(f'  {100 * n / total:5.1f}%  ' + ' > '.join(label(f) for f in p))
    print(f'\ncallees to depth {down} (inclusive share of its samples, >= {min_pct}%):')

    def show(prefix, depth):
        kids = [(k, n) for k, n in tree.items() if len(k) == depth + 1 and k[:depth] == prefix]
        for k, n in sorted(kids, key=lambda kv: -kv[1]):
            pct = 100 * n / total
            if pct < min_pct:
                continue
            print(f'  {"  " * depth}{pct:5.1f}%  {label(k[-1])}')
            if depth + 1 < down:
                show(k, depth + 1)
    show((), 0)


if __name__ == '__main__':
    main()
