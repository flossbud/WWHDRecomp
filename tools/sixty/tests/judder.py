"""judder.py DIR PROC FROM TO [OFFSET...]: in a 60 fps run's track (DIR/track.bin), how evenly a vector of
process PROC moves from frame to frame between game frames FROM and TO: for each OFFSET (hex; default
0x314, the position; 0x390 is the eye point) the mean move made by whole-tick frames and by half-tick
frames, per axis and in length, and how often a frame moves against the one before (a sign flip on an
axis that is moving). A converted actor that judders moves unevenly: the two kinds of frame differ, or
it steps back every other frame (session qa: Link on stairs, his eye 250, 237.6, 246.4). At N steps a tick (WWHD_STEPS,
U4) the track keeps a tick's whole step and its last: a half-tick key is (N - 1)/N of a tick after the whole one and
1/N before the next, so each kind's move is also given per tick (even motion: the same for both)."""
import math
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import compare  # noqa: E402

d, proc, a, b = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
offs = [int(x, 16) for x in sys.argv[5:]] or [0x314]
steps = int(os.environ["WWHD_STEPS"]) if os.environ.get("WWHD_STEPS", "").isdigit() else 2   # (display: the frame log's "# steps N")
span = {"half": (steps - 1) / steps, "whole": 1 / steps}     # the share of a tick each kind of move covers
T = compare.load_track(d + "/track.bin")
recs = {}
for k in T:
    if k[0] == proc:
        recs.update(T[k])
ticks = [t for t in range(2 * a, 2 * b + 1) if t in recs]
for off in offs:
    v = {t: struct.unpack(">3f", recs[t][off:off + 12]) for t in ticks if len(recs[t]) >= off + 12}
    moves = {"whole": [], "half": []}
    flips = [0, 0, 0]
    last = None
    for p, c in zip(ticks, ticks[1:]):
        if c != p + 1 or p not in v or c not in v:
            last = None
            continue
        m = [v[c][i] - v[p][i] for i in range(3)]
        moves["whole" if c % 2 == 0 else "half"].append(m)
        if last is not None:
            for i in range(3):
                if m[i] * last[i] < 0 and min(abs(m[i]), abs(last[i])) > 0.05:
                    flips[i] += 1
        last = m
    print(f"+{off:#x}: {len(ticks)} frames; frames moving against the one before: x {flips[0]}, y {flips[1]}, z {flips[2]}")
    for kind, ms in moves.items():
        if not ms:
            continue
        n = len(ms)
        mean = [sum(m[i] for m in ms) / n for i in range(3)]
        length = sum(math.sqrt(sum(c * c for c in m)) for m in ms) / n
        print(f"   {kind:5} frames: mean move ({mean[0]:8.3f} {mean[1]:8.3f} {mean[2]:8.3f}), mean length {length:.3f}, "
              f"{length / span[kind]:.3f} a tick  ({n} moves)")
