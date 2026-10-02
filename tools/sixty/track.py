"""A tracked process tick by tick, the 30-tick run beside the 60-tick run (docs/recompiler-design.md D21).

Usage (on the worker):
    python3 tools/sixty/track.py A_DIR B_DIR NAME [FIELD...] [--from T] [--to T] [--every N] [--half]

A_DIR and B_DIR hold track.bin from runs with WWHD_STATE_TRACK (normally OUT/30 and OUT/60 of
tools/sixty/run.sh). NAME is a process name (168 Link, 476 the camera). Each FIELD is OFFSET:TYPE,
TYPE f (a single), h (s16), H (u16), i (s32), b (u8): for example 0x370:f for speedF; the position
(+0x314) and its error are always shown. Prints each whole tick from --from to --to (every --every;
with --half the 60-tick run's half ticks too, its values beside nothing), and the first ticks the
position error passes 0.01, 0.1, 1, 10 and 100 units. The output is game state: keep it on the worker.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import compare  # noqa: E402

FMT = {"f": (">f", 4), "h": (">h", 2), "H": (">H", 2), "i": (">i", 4), "b": (">B", 1)}


def value(data, off, kind):
    fmt, size = FMT[kind]
    if off + size > len(data):
        return None
    return struct.unpack(fmt, data[off:off + size])[0]


def show(v, kind):
    if v is None:
        return "-"
    return f"{v:.4f}" if kind == "f" else str(v)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("name", type=int)
    ap.add_argument("fields", nargs="*")
    ap.add_argument("--from", dest="first", type=int, default=0)
    ap.add_argument("--to", dest="last", type=int, default=1 << 30)
    ap.add_argument("--every", type=int, default=1)
    ap.add_argument("--half", action="store_true")
    args = ap.parse_args()
    fields = []
    for f in args.fields:
        off, _, kind = f.partition(":")
        fields.append((int(off, 0), kind or "f"))
    ta, tb = compare.load_track(f"{args.a}/track.bin"), compare.load_track(f"{args.b}/track.bin")
    keys = sorted(set(k for k in ta if k[0] == args.name) & set(k for k in tb if k[0] == args.name))
    if not keys:
        sys.exit(f"process {args.name} isn't in both runs' track.bin")
    for key in keys:
        a, b = ta[key], tb[key]
        print(f"process {key[0]} at {key[1]}:")
        print("tick      err        " + "  ".join(f"+{o:#x}:{k} 30 / 60" for o, k in fields))
        passed = {}
        for t2 in sorted(set(a) | set(b)):
            t = t2 // 2
            if t < args.first or t > args.last:
                continue
            half = t2 % 2 == 1
            x, y = a.get(t2), b.get(t2)
            err = None
            if x is not None and y is not None:
                err = sum((value(x, 0x314 + 4 * i, "f") - value(y, 0x314 + 4 * i, "f")) ** 2 for i in range(3)) ** 0.5
                for th in (0.01, 0.1, 1, 10, 100):
                    if err > th and th not in passed:
                        passed[th] = t
            if (half and not args.half) or (not half and t % args.every):
                continue
            cells = []
            for off, kind in fields:
                va = value(x, off, kind) if x is not None else None
                vb = value(y, off, kind) if y is not None else None
                cells.append(f"{show(va, kind)} / {show(vb, kind)}")
            label = f"{t}.5" if half else f"{t}"
            print(f"{label:9s} {('-' if err is None else f'{err:.3f}'):10s} " + "  ".join(cells))
        print("first tick the position error passes:", ", ".join(f"{k}: {v}" for k, v in sorted(passed.items())))


if __name__ == "__main__":
    main()
