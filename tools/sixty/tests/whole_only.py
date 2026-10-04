"""whole_only.py DIR PROC FROM TO [MIN]: in a 60 fps run's track (DIR/track.bin, WWHD_STATE_TRACK), the
fields of process PROC that change on whole ticks only: 4-byte words that changed at least MIN times
(default 8) at whole ticks between game frames FROM and TO and never at a half tick. A converted
process's field that moves at 30 while the rest moves at 60 (something counted or copied once a tick,
or set from an unconverted process): what looks 30 fps in an otherwise smooth actor. Prints the offset,
how often it changed, and a few values as floats and as integers."""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import compare  # noqa: E402

d, proc, a, b = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
least = int(sys.argv[5]) if len(sys.argv) > 5 else 8
T = compare.load_track(d + "/track.bin")
recs = {}
for k in T:
    if k[0] == proc:
        recs.update(T[k])
ticks = [t for t in range(2 * a, 2 * b + 1) if t in recs]
size = min(len(recs[t]) for t in ticks) & ~3
whole = [0] * (size // 4)
half = [0] * (size // 4)
for prev, cur in zip(ticks, ticks[1:]):
    if cur != prev + 1:
        continue
    x, y = recs[prev], recs[cur]
    if x == y:
        continue
    count = whole if cur % 2 == 0 else half          # the frame that made the change: cur
    for o in range(0, size, 4):
        if x[o:o + 4] != y[o:o + 4]:
            count[o // 4] += 1
both = sum(1 for w, h in zip(whole, half) if w and h)
print(f"{len(ticks)} frames; {both} words change on both kinds of frame")
for i, (w, h) in enumerate(zip(whole, half)):
    if w >= least and h == 0:
        o = 4 * i
        vals = [recs[t][o:o + 4] for t in ticks[:: max(1, len(ticks) // 6)]][:6]
        print(f"+{o:04x}  {w:3d} whole-tick changes, 0 half:  "
              + " ".join("%.4g" % struct.unpack(">f", v)[0] for v in vals) + "  | " + " ".join(v.hex() for v in vals[:3]))
