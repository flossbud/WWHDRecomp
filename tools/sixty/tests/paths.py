"""paths.py DIR FROM TO [OFFSET]: each tracked actor's position (or the vector at OFFSET, e.g. 0x390 for
a rooted actor's eye point) at 30's tick k against 60's half frame k (D21): how far apart (mean,
max) and each run's path length. DIR has 30/ and 60/ from tools/sixty/run.sh with WWHD_STATE_TRACK."""
import math
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import compare  # noqa: E402

d, a, b = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
off = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x314
A = compare.load_track(d + "/30/track.bin")
B = compare.load_track(d + "/60/track.bin")
f3 = lambda x: struct.unpack(">3f", x[off:off + 12])
for k in A:
    if k not in B:
        continue
    ds = []
    pa = pb = 0
    for t in range(a + 1, b):
        x, y = A[k].get(2 * t), B[k].get(2 * t + 1)
        x0, y0 = A[k].get(2 * t - 2), B[k].get(2 * t - 1)
        if x and y and len(x) >= off + 12 and len(y) >= off + 12:
            ds.append(math.dist(f3(x), f3(y)))
        if x and x0 and len(x0) >= off + 12:
            pa += math.dist(f3(x), f3(x0))
        if y and y0 and len(y0) >= off + 12:
            pb += math.dist(f3(y), f3(y0))
    if ds:
        print(k, "apart mean %.1f max %.1f; path 30 %.0f, 60 %.0f; states %d" % (sum(ds) / len(ds), max(ds), pa, pb, len(ds)))
