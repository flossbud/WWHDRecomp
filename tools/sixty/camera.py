"""The camera's view at 60 fps against the 30-tick run's (docs/recompiler-design.md D21).

Usage (on the worker):
    python3 tools/sixty/camera.py A_DIR B_DIR [--from T] [--worst N]

A_DIR and B_DIR hold track.bin from runs with WWHD_STATE_TRACK including 476 (the camera process;
normally OUT/30 and OUT/60 of tools/sixty/run.sh). The camera process keeps the view it hands the
renderer at +0xD4 (fovy), +0xDC (eye) and +0xE8 (center). For each camera process in both runs this
prints, over the whole ticks from --from: the eye's distance between the runs and the angle between
their view directions (eye - center), mean and largest, and the --worst ticks by angle with both
runs' yaw and distance. The output is game state: keep it on the worker.
"""
import argparse
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import compare  # noqa: E402


def vec(d, off):
    return [struct.unpack(">f", d[off + 4 * i:off + 4 * i + 4])[0] for i in range(3)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--from", dest="first", type=int, default=900)
    ap.add_argument("--worst", type=int, default=5)
    args = ap.parse_args()
    ta, tb = compare.load_track(f"{args.a}/track.bin"), compare.load_track(f"{args.b}/track.bin")
    for key in sorted(k for k in ta if k[0] == 476 and k in tb):
        a, b = ta[key], tb[key]
        rows = []
        for t in sorted(t for t in a if t in b and t % 2 == 0 and t // 2 >= args.first):
            ea, eb, ca, cb = vec(a[t], 0xDC), vec(b[t], 0xDC), vec(a[t], 0xE8), vec(b[t], 0xE8)
            va = [ea[i] - ca[i] for i in range(3)]
            vb = [eb[i] - cb[i] for i in range(3)]
            na, nb = math.hypot(*va), math.hypot(*vb)
            if not na or not nb:
                continue
            cos = sum(va[i] * vb[i] for i in range(3)) / (na * nb)
            angle = math.degrees(math.acos(max(-1.0, min(1.0, cos))))
            rows.append((t // 2, math.dist(ea, eb), angle, math.degrees(math.atan2(va[0], va[2])),
                         math.degrees(math.atan2(vb[0], vb[2])), na, nb))
        if not rows:
            continue
        n = len(rows)
        print(f"camera at {key[1]}: {n} whole ticks; eye apart mean {sum(r[1] for r in rows) / n:.1f}, "
              f"largest {max(r[1] for r in rows):.1f}; view direction apart mean {sum(r[2] for r in rows) / n:.2f} deg, "
              f"largest {max(r[2] for r in rows):.2f} deg")
        for t, de, ang, ya, yb, na, nb in sorted(rows, key=lambda r: -r[2])[:args.worst]:
            print(f"  tick {t}: {ang:.2f} deg apart (yaw {ya:.1f} / {yb:.1f}, distance {na:.0f} / {nb:.0f}), eye {de:.1f} apart")


if __name__ == "__main__":
    main()
