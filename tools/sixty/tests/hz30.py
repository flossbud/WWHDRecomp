"""What on screen still moves at 30 Hz at 60 fps: screen blocks that change only every other swap.
Usage (on the worker; the captures are game data, so its output stays there):
    tools/sixty/tests/capture.sh NAME STAGE,point,room,layer $(seq -s, 1300 1359)
    python3 tools/sixty/tests/hz30.py $SIXTY_OUT/cap-NAME [BLOCK_PX]
In a view held still (no input after the warp), something converted (or a value a draw steps each frame)
changes on every swap at 60 fps, and something still stepping once a tick changes on one swap of each
pair only. Per block of BLOCK_PX (default 24) pixels: of the swap pairs where it changed (over 2% of its
pixels by more than 12), the share on one parity. Listed: blocks changing in over a quarter of the pairs,
85% or more on one parity. To see what a block is, a difference image of two swaps
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
    frames = [load(p) for p in sorted(glob.glob(os.path.join(d, "shots", "f*.tv.ppm")))]
    h, w = frames[0].shape[:2]
    by, bx = h // size, w // size
    changed = np.zeros((len(frames) - 1, by, bx), bool)
    for i in range(len(frames) - 1):
        moved = np.abs(frames[i + 1] - frames[i]).max(axis=2) > 12
        changed[i] = moved[:by * size, :bx * size].reshape(by, size, bx, size).mean(axis=(1, 3)) > 0.02
    even, odd = changed[0::2].sum(axis=0), changed[1::2].sum(axis=0)
    pairs = len(frames) - 1
    total = even + odd
    share = np.where(total > 0, np.maximum(even, odd) / np.maximum(total, 1), 0)
    found = sorted(((share[y, x], total[y, x], y, x) for y in range(by) for x in range(bx)
                    if total[y, x] >= pairs // 4 and share[y, x] > 0.85), reverse=True)
    print(f"{len(frames)} swaps, {by}x{bx} blocks of {size}px; changing blocks {int((total > 0).sum())}; "
          f"at 30 Hz (>85% on one parity, changing in >1/4 of pairs): {len(found)}")
    for s, t, y, x in found[:20]:
        print(f"  block at x {x * size}-{x * size + size} y {y * size}-{y * size + size}: {t} changes, "
              f"{100 * s:.0f}% on one parity (even {even[y, x]}, odd {odd[y, x]})")


if __name__ == "__main__":
    main()
