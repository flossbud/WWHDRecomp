"""Compare the state probe's dumps of two runs at equal game frames (docs/recompiler-design.md D21).

Usage (on the worker, next to the dumps: they are game memory):
    python3 tools/sixty/compare.py A_DIR B_DIR [--from TICK] [--fields N] [--names NAMES.tsv]

A_DIR and B_DIR are WWHD_STATE_DUMP directories (tools/sixty/run.sh writes OUT/30 and OUT/60). Ticks
are game frames (src/overrides/sixty.cpp). It prints:
  - the game clock at matching ticks (ticks.txt), so time-driven state can be told apart;
  - per tick from --from on, how many actors match (FNV-1a of their bytes), differ, or exist in one
    run only (actors are keyed by process name and address);
  - per process name, the ticks where it differed (count, first);
  - per (process name, offset), from the full dumps (state.bin, every WWHD_STATE_DUMP_EVERY ticks),
    the 32-bit words that differ: in how many dumps, the first tick, and the values at that tick as
    float and as integer: the list of per-tick quantities to look at, longest-standing first.
With --track, only the processes WWHD_STATE_TRACK dumped every tick (track.bin): converted ones
within a tolerance (their position error at whole ticks, the floats that differ most, and at half
ticks how far they are from the midpoint of the 30-tick run's two ticks around them).
--names maps process names to readable names (a TSV: number, name).
"""
import argparse
import collections
import struct


def load_hashes(path):
    by_tick = collections.defaultdict(dict)
    for line in open(path):
        t, n, addr, size, h = line.split()
        by_tick[int(t)][(int(n), addr)] = h
    return by_tick


def load_ticks(path):
    out = {}
    for line in open(path):
        t, swap, tb = line.split()
        out[int(t)] = (int(swap), int(tb))
    return out


def load_state(path):
    """(tick, kind, a) -> (ea, bytes); kind 'GLOB' (a = index) or 'ACTR' (a = (name, ea))."""
    out = {}
    with open(path, 'rb') as f:
        while True:
            head = f.read(24)
            if len(head) < 24:
                break
            tag, tick, a, b, ea, size = struct.unpack('<6I', head)
            data = f.read(size)
            if tag == 0x474C4F42:
                out[(tick, 'GLOB', a)] = (ea, data)
            elif tag == 0x41435452:
                out[(tick, 'ACTR', (a, f'{ea:08x}'))] = (ea, data)
    return out


def word_diffs(x, y):
    n = min(len(x), len(y)) // 4 * 4
    for off in range(0, n, 4):
        if x[off:off + 4] != y[off:off + 4]:
            yield off, x[off:off + 4], y[off:off + 4]


def show(w):
    i = struct.unpack('>I', w)[0]
    f = struct.unpack('>f', w)[0]
    fs = f'{f:.6g}' if abs(f) < 1e9 and (f == 0 or abs(f) > 1e-9) else '-'
    return f'{i:08x} ({fs})'


def load_track(path):
    """{(name, addr): {tick2: bytes}} from track.bin (WWHD_STATE_TRACK)."""
    out = collections.defaultdict(dict)
    try:
        f = open(path, 'rb')
    except FileNotFoundError:
        return out
    with f:
        while True:
            head = f.read(16)
            if len(head) < 16:
                break
            tick2, name, addr, size = struct.unpack('<4I', head)
            out[(name, f'{addr:08x}')][tick2] = f.read(size)
    return out


def f32(b, off):
    return struct.unpack('>f', b[off:off + 4])[0]


def track_report(a_dir, b_dir, names, first):
    """Converted processes within a tolerance: at whole ticks the position (+0x314) error and the
    float fields that differ most; at half ticks (B only) how far the position is from the midpoint
    of A's two whole ticks around it, against the distance moved in that tick."""
    ta, tb = load_track(f'{a_dir}/track.bin'), load_track(f'{b_dir}/track.bin')
    if not tb:
        print('\nno track.bin in B (WWHD_STATE_TRACK)')
        return
    print('\ntracked processes (positions at +0x314; units: the game\'s):')
    for key in sorted(set(ta) & set(tb)):
        a, b = ta[key], tb[key]
        whole = sorted(t for t in a if t in b and t % 2 == 0 and t // 2 >= first)
        if not whole:
            continue
        worst, worst_t = 0.0, None
        fields = collections.defaultdict(float)
        for t in whole:
            x, y = a[t], b[t]
            d = sum((f32(x, 0x314 + 4 * i) - f32(y, 0x314 + 4 * i)) ** 2 for i in range(3)) ** 0.5
            if d > worst:
                worst, worst_t = d, t // 2
            for off in range(0, min(len(x), len(y)) - 3, 4):
                if x[off:off + 4] != y[off:off + 4]:
                    fx, fy = f32(x, off), f32(y, off)
                    if abs(fx) < 1e7 and abs(fy) < 1e7 and (fx != 0 or fy != 0):
                        fields[off] = max(fields[off], abs(fx - fy))
        mids, moved = [], []
        for t in sorted(t for t in b if t % 2 == 1 and t // 2 >= first):
            n = t // 2
            if 2 * n in a and 2 * (n + 1) in a:
                p0 = [f32(a[2 * n], 0x314 + 4 * i) for i in range(3)]
                p1 = [f32(a[2 * (n + 1)], 0x314 + 4 * i) for i in range(3)]
                ph = [f32(b[t], 0x314 + 4 * i) for i in range(3)]
                mids.append(sum((ph[i] - (p0[i] + p1[i]) / 2) ** 2 for i in range(3)) ** 0.5)
                moved.append(sum((p1[i] - p0[i]) ** 2 for i in range(3)) ** 0.5)
        name = key[0]
        print(f'  {name} at {key[1]}: {len(whole)} whole ticks, position error max {worst:.3f} (tick {worst_t})')
        if mids:
            big = max(range(len(mids)), key=lambda i: mids[i])
            print(f'    half ticks: {len(mids)}, distance from the midpoint max {mids[big]:.3f} '
                  f'(moved {moved[big]:.3f} that tick), mean {sum(mids) / len(mids):.3f}')
        top = sorted(fields.items(), key=lambda kv: -kv[1])[:8]
        if top:
            print('    floats that differ most: ' + ', '.join(f'+{o:#x} {d:.4g}' for o, d in top))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('--from', dest='first', type=int, default=0)
    ap.add_argument('--fields', type=int, default=60)
    ap.add_argument('--names')
    ap.add_argument('--globals-only', action='store_true', help='only the globals in the full dumps')
    ap.add_argument('--track', action='store_true', help='only the tracked processes (track.bin), within a tolerance')
    args = ap.parse_args()
    if args.track:
        track_report(args.a, args.b, args.names, args.first)
        return
    names = {}
    if args.names:
        for line in open(args.names):
            p = line.rstrip('\n').split('\t')
            if len(p) >= 2 and p[0].isdigit():
                names[int(p[0])] = p[1]

    def nm(n):
        return f'{n} {names[n]}' if n in names else str(n)

    ha, hb = load_hashes(f'{args.a}/hashes.txt'), load_hashes(f'{args.b}/hashes.txt')
    ta, tb = load_ticks(f'{args.a}/ticks.txt'), load_ticks(f'{args.b}/ticks.txt')
    ticks = sorted(t for t in ha if t in hb and t >= args.first)
    if not ticks:
        raise SystemExit('no common ticks')
    print(f'{len(ticks)} common ticks, {ticks[0]}..{ticks[-1]}')
    for t in ticks[:: max(1, len(ticks) // 8)] + [ticks[-1]]:
        if t in ta and t in tb:
            d = (tb[t][1] - ta[t][1]) / 62156.25
            print(f'  tick {t}: swaps {ta[t][0]} / {tb[t][0]}, game clock {ta[t][1] / 62156250:.3f} s, B - A {d:+.2f} ms')

    per_name = collections.defaultdict(lambda: [0, None])
    rows = []
    for t in ticks:
        a, b = ha[t], hb[t]
        same = sum(1 for k in a if k in b and a[k] == b[k])
        diff = [k for k in a if k in b and a[k] != b[k]]
        only_a = [k for k in a if k not in b]
        only_b = [k for k in b if k not in a]
        rows.append((t, len(a), same, len(diff), len(only_a), len(only_b)))
        for k in diff + only_a + only_b:
            e = per_name[k[0]]
            e[0] += 1
            if e[1] is None:
                e[1] = t
    print('\nper tick (every 1/16th, and the first with a difference): actors, same, differ, only A, only B')
    first_bad = next((r for r in rows if r[3] or r[4] or r[5]), None)
    for r in rows[:: max(1, len(rows) // 16)] + ([first_bad] if first_bad else []):
        print(f'  {r[0]:6d}: {r[1]:4d} {r[2]:4d} {r[3]:4d} {r[4]:4d} {r[5]:4d}')
    if not first_bad:
        print('\nevery actor matches at every common tick')
        return
    print(f'\nper process name: ticks with a difference, first tick ({len(per_name)} names)')
    for n, (count, first) in sorted(per_name.items(), key=lambda kv: (kv[1][1], -kv[1][0])):
        print(f'  {nm(n):>28s}: {count:5d} ticks, first {first}')

    sa, sb = load_state(f'{args.a}/state.bin'), load_state(f'{args.b}/state.bin')
    fields = collections.defaultdict(lambda: [0, None, None, None])
    dumps = 0
    for key in sa:
        if key not in sb or key[0] < args.first or (args.globals_only and key[1] != 'GLOB'):
            continue
        dumps += 1
        (_, x), (_, y) = sa[key], sb[key]
        who = ('global', key[2]) if key[1] == 'GLOB' else key[2][0]
        ea = sa[key][0]
        for off, wx, wy in word_diffs(x, y):
            if key[1] == 'GLOB':                   # globals by address
                who, off = ('global', 0), ea + off
            e = fields[(who, off)]
            e[0] += 1
            if e[1] is None:
                e[1], e[2], e[3] = key[0], wx, wy
    print(f'\nwords that differ in the full dumps ({dumps} records compared): name +offset: dumps, first tick, A, B')
    for (who, off), (count, first, wx, wy) in sorted(fields.items(), key=lambda kv: (-kv[1][0], kv[1][1]))[: args.fields]:
        if isinstance(who, tuple):
            print(f'  {"global":>28s} {off:08x}: {count:4d}, first {first:6d}: {show(wx)} vs {show(wy)}')
        else:
            print(f'  {nm(who):>28s} +{off:#06x}: {count:4d}, first {first:6d}: {show(wx)} vs {show(wy)}')


if __name__ == '__main__':
    main()
