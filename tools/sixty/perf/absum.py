"""absum.py DIR ROUTE: the A/B runs (perf-ab.sh) side by side, gameplay only (game frame 900 on): per run the
fps, the scheduler's CPU (% of a core), whole and half frames' CPU (median, 90th, ms), late frames and the
journal's stores (session top, WW-4 perf; timings only)."""
import glob, os, re, statistics, sys

def pct(v, q):
    v = sorted(v)
    return v[min(len(v) - 1, int(q * len(v)))] if v else float("nan")

d, route = sys.argv[1], sys.argv[2]
for path in sorted(glob.glob(os.path.join(d, f"ab-*-{route}-*.frames"))):
    fr = []
    for line in open(path):
        p = line.split()
        if line.startswith("#") or int(p[0]) // 2 < 900:
            continue
        fr.append((p[1] == "h", float(p[2]), float(p[3]), float(p[5]), int(p[8]), float(p[9]), int(p[10]) if len(p) > 11 else 0))
    if len(fr) < 2:
        continue
    span = (fr[-1][1] + fr[-1][2] + fr[-1][5] - fr[0][1]) / 1000
    w = [f[3] for f in fr if not f[0]]; h = [f[3] for f in fr if f[0]]
    print(f"{os.path.basename(path):24s} {len(fr) / span:5.1f} fps, scheduler {sum(f[3] for f in fr) / span / 10:5.1f}%,"
          f" whole cpu {statistics.median(w):5.2f}/{pct(w, 0.9):5.2f}, half cpu {statistics.median(h):5.2f}/{pct(h, 0.9):5.2f} ms,"
          f" late {100 * sum(1 for f in fr if f[4] > 0) / len(fr):4.1f}%, journal {statistics.median(f[6] for f in fr if f[0])}k")
