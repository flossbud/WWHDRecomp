"""perfsum.py DIR [ROUTE...]: the perf runs (perf-top.sh) at 30 against 60, gameplay only (game frame 900 on):
fps, the scheduler and GPU threads' CPU per wall second, whole and half frames' work and CPU (median, 90th),
late frames, the half frames' journal, and every thread's CPU (session top, WW-4 perf; timings only)."""
import os, statistics, sys

def pct(v, q):
    v = sorted(v)
    return v[min(len(v) - 1, int(q * len(v)))] if v else float("nan")

def load(path, rate):
    rows = []
    for line in open(path):
        if line.startswith("#"):
            continue
        p = line.split()
        swap = int(p[0]); game = swap if rate == 30 else swap // 2
        if game < 900:
            continue
        f = {"half": p[1] == "h", "began": float(p[2]), "work": float(p[3]), "drawdone": float(p[4]), "cpu": float(p[5]),
             "gpu": float(p[7]), "vsyncs": int(p[8]), "wait": float(p[9])}
        if len(p) > 11:
            f["stores"], f["saved"] = int(p[10]), int(p[11])
        rows.append(f)
    return rows

d = sys.argv[1]
routes = sys.argv[2:] or ["continue", "tour", "sail", "menus", "warp"]
for r in routes:
    print(f"== {r}")
    for rate in (30, 60):
        path = os.path.join(d, f"{rate}-{r}.frames")
        if not os.path.exists(path):
            print(f"  {rate}: -"); continue
        fr = load(path, rate)
        if len(fr) < 2:
            print(f"  {rate}: too few frames"); continue
        span = (fr[-1]["began"] + fr[-1]["work"] + fr[-1]["wait"] - fr[0]["began"]) / 1000
        cpu = sum(f["cpu"] for f in fr) / span / 10      # % of a core
        gpu = sum(f["gpu"] for f in fr) / span / 10
        late = 100 * sum(1 for f in fr if f["vsyncs"] > 0) / len(fr)
        line = f"  {rate}: {len(fr) / span:5.1f} fps over {span:5.1f} s; scheduler {cpu:5.1f}% of a core, gpu thread {gpu:5.1f}%; late {late:4.1f}%"
        for kind, sel in (("whole", [f for f in fr if not f["half"]]), ("half", [f for f in fr if f["half"]])):
            if sel:
                line += (f"\n      {kind}: work {statistics.median(f['work'] for f in sel):5.2f}/{pct([f['work'] for f in sel], 0.9):5.2f} ms,"
                         f" cpu {statistics.median(f['cpu'] for f in sel):5.2f}/{pct([f['cpu'] for f in sel], 0.9):5.2f} ms,"
                         f" drawdone {statistics.median(f['drawdone'] for f in sel):4.2f}, gpu {statistics.median(f['gpu'] for f in sel):5.2f}")
                if kind == "half" and "stores" in sel[0]:
                    line += f", journal {statistics.median(f['stores'] for f in sel)}k stores / {statistics.median(f['saved'] for f in sel)}k saved"
        print(line)
        t = os.path.join(d, f"{rate}-{r}.threads")
        if os.path.exists(t):
            print("      threads: " + open(t).read().strip())
