"""Glitch frames in a series of consecutive captures: frames that differ from both neighbours while the
neighbours agree with each other (session qa, the hunt for bug B16's streaks).

Usage (on the worker, after tools/sixty/tests/capture.sh NAME STAGE SWAPS...):
    python3 tools/sixty/tests/streak.py CAPDIR [K...]

CAPDIR/shots/f*.tv.ppm are captures of consecutive swaps (or every nth). For each spacing K (default 1 4)
a frame scores min(change from the frame K before, change to the frame K after) - change between those
two, each a share of the pixels (of a third-size image) that differ by more than 48 in a channel. Something
that appears for a frame or a few and vanishes scores high; steady motion (a walking crowd, a turning
camera) scores near or below 0. Prints the eight top frames per K. Seen so far: 0.036 for people walking
on Windfall, <= 0.002 on still night views; Link swimming under a dock scored 0.20 at K=12 (his head).
The captures are game data: they stay on the worker.
"""
import sys, glob, os
import numpy as np
d = sys.argv[1]
ks = [int(x) for x in sys.argv[2:]] or [1, 4]
files = sorted(glob.glob(os.path.join(d, "shots", "f*.tv.ppm")))
def load(p):
    with open(p, "rb") as f:
        assert f.readline().strip() == b"P6"
        w, h = map(int, f.readline().split()); f.readline()
        a = np.frombuffer(f.read(w * h * 3), dtype=np.uint8).reshape(h, w, 3)
    return a[::3, ::3].astype(np.int16)
fr = [load(p) for p in files]
n = len(fr)
def m(a, b): return float((np.abs(a - b).max(axis=2) > 48).mean())
print(len(files), "frames", fr[0].shape)
for k in ks:
    out = []
    for i in range(k, n - k):
        a, b, c = m(fr[i - k], fr[i]), m(fr[i], fr[i + k]), m(fr[i - k], fr[i + k])
        out.append((min(a, b) - c, i, a, b, c))
    out.sort(reverse=True)
    print("k=%d: top anomalies (score = min(change before, change after) - change across), as a share of pixels:" % k)
    for s, i, a, b, c in out[:8]:
        print("   %s score %.4f (before %.4f after %.4f across %.4f)" % (os.path.basename(files[i]), s, a, b, c))
