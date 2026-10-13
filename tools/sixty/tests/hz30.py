"""What on screen still moves at 30 Hz at 60 fps: screen blocks that change only every other swap.
Usage (on the worker; the captures are game data, so its output stays there):
    tools/sixty/tests/capture.sh NAME STAGE,point,room,layer $(seq -s, 1300 1359)
    python3 tools/sixty/tests/hz30.py $SIXTY_OUT/cap-NAME [BLOCK_PX] [N]   (N: frames a tick, WWHD_STEPS; 2 at 60)
In a view held still (no input after the warp), something converted (or a value a draw steps each frame)
changes on every swap at 60 fps, and something still stepping once a tick changes on one swap of each
pair only. Per block of BLOCK_PX (default 24) pixels: of the swap pairs where it changed (over 2% of its
pixels by more than 12), the share on one parity. Listed: blocks changing in half the ticks or more,
85% or more on one parity. At N frames a tick (U4), by the frame's place in its tick: from N 4 on, 85% or more on
N/2 of the N phases (a step left at 60's rhythm shows too). To see what a block is, a difference image of two swaps
(`convert A B -compose difference -composite`). Found this way (session top, 2026-10-05): the sea's foam
(the sea process and its draw, tick_rules.txt), the Wind Temple's dust motes and the weather (session
bottom's "weather" item). The HUD's corner (a block near x 1776-1800 y 96-120) shows on every scan.
"""
import glob
import os
import sys

import numpy as np


def load(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        w, h = map(int, f.readline().split())
        f.readline()
        return np.frombuffer(f.read(w * h * 3), dtype=np.uint8).reshape(h, w, 3).astype(np.int16)


def main():
    d = sys.argv[1]
    size = int(sys.argv[2]) if len(sys.argv) > 2 else 24
    steps = int(sys.argv[3]) if len(sys.argv) > 3 else (int(os.environ["WWHD_STEPS"]) if os.environ.get("WWHD_STEPS", "").isdigit() else 2)   # frames a tick (U4)
    frames = [load(p) for p in sorted(glob.glob(os.path.join(d, "shots", "f*.tv.ppm")))]
    h, w = frames[0].shape[:2]
    by, bx = h // size, w // size
    changed = np.zeros((len(frames) - 1, by, bx), bool)
    for i in range(len(frames) - 1):
        moved = np.abs(frames[i + 1] - frames[i]).max(axis=2) > 12
        changed[i] = moved[:by * size, :bx * size].reshape(by, size, bx, size).mean(axis=(1, 3)) > 0.02
    # by the swap's place in its tick (U4: N phases at WWHD_STEPS=N; parity at 2): something stepping once a tick
    # changes on one phase only; from N 4 on, on half the phases or fewer (also a step left at 60's rhythm, every
    # other frame)
    phases = np.stack([changed[p::steps].sum(axis=0) for p in range(steps)])
    k = max(1, steps // 2)
    pairs = (len(frames) - 1) * 2 // steps
    total = phases.sum(axis=0)
    share = np.where(total > 0, np.sort(phases, axis=0)[::-1][:k].sum(axis=0) / np.maximum(total, 1), 0)
    found = sorted(((share[y, x], total[y, x], y, x) for y in range(by) for x in range(bx)
                    if total[y, x] >= pairs // 4 and share[y, x] > 0.85), reverse=True)
    on = "one" if k == 1 else f"{k}"
    print(f"{len(frames)} swaps, {by}x{bx} blocks of {size}px; changing blocks {int((total > 0).sum())}; "
          f"stepping by the tick (>85% on {on} of {steps} phases, changing in half the ticks or more): {len(found)}")
    for s, t, y, x in found[:20]:
        print(f"  block at x {x * size}-{x * size + size} y {y * size}-{y * size + size}: {t} changes, "
              f"{100 * s:.0f}% on {on} phase{'s' if k > 1 else ''} ({', '.join(str(int(c)) for c in phases[:, y, x])})")


if __name__ == "__main__":
    main()
