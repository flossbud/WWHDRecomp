"""worker_absum.py DIR [ROUTE]: worker-ab.sh's runs summarised, gameplay only (game frame 900 on; round 0, the
warm-up, left out). Per run: the fps, the scheduler (game) thread's and the render thread's CPU (% of a core, from
the frame log), the game thread's work a frame (wall, to its wait: GX2DrawDone's wait included) and CPU a frame, the
render thread's CPU a frame (mean and median, ms), the DrawDone wait, late frames, and the threads' CPU (.threads).
Then per variant the median over its runs, and each variant against the first, round by round (session cloud;
timings only)."""
import glob, os, re, statistics, sys

d = sys.argv[1]
route = sys.argv[2] if len(sys.argv) > 2 else "*"
rate_of = {}
try:   # the variants line: "SHA name:K=V,... name..." (RATE=30 marks a 30 fps variant)
    for v in open(os.path.join(d, "variants")).read().split()[1:]:
        name, _, kvs = v.partition(":")
        rate_of[name] = 30 if "RATE=30" in kvs.split(",") else 60
except OSError:
    pass

def pct(v, q):
    v = sorted(v)
    return v[min(len(v) - 1, int(q * len(v)))] if v else float("nan")

def run(path, rate):
    fr = []
    for line in open(path):
        if line.startswith("#"):
            continue
        p = line.split()
        if (int(p[0]) // (2 if rate == 60 else 1)) < 900:
            continue
        fr.append(dict(half=p[1] == "h", began=float(p[2]), work=float(p[3]), drawdone=float(p[4]), cpu=float(p[5]),
                       gpu=float(p[7]), vsyncs=int(p[8]), wait=float(p[9])))
    if len(fr) < 10:
        return None
    span = (fr[-1]["began"] + fr[-1]["work"] + fr[-1]["wait"] - fr[0]["began"]) / 1000
    m = lambda k: statistics.mean(f[k] for f in fr)
    r = dict(fps=len(fr) / span, sched=sum(f["cpu"] for f in fr) / span / 10, render=sum(f["gpu"] for f in fr) / span / 10,
             work=m("work"), cpu=m("cpu"), gpu=m("gpu"), drawdone=m("drawdone"),
             work_med=statistics.median(f["work"] for f in fr), gpu_med=statistics.median(f["gpu"] for f in fr),
             late=100 * sum(1 for f in fr if f["vsyncs"] > 0) / len(fr), n=len(fr))
    t = path[:-len(".frames")] + ".threads"
    r["threads"] = open(t).read().strip() if os.path.exists(t) else ""
    return r

runs = {}   # name -> {round: stats}
for path in sorted(glob.glob(os.path.join(d, f"ab-*-{route}-*.frames"))):
    m = re.match(r"ab-(.+)-([^-]+)-(\d+)\.frames$", os.path.basename(path))
    if not m or m.group(3) == "0":
        continue
    name = m.group(1)
    r = run(path, rate_of.get(name, 60))
    if r:
        runs.setdefault(name, {})[int(m.group(3))] = r
        print(f"{name:>10s} {m.group(3):>2s}: {r['fps']:5.1f} fps, game thread {r['sched']:5.1f}%, render {r['render']:5.1f}%;"
              f" a frame: work {r['work']:5.2f} (med {r['work_med']:5.2f}), cpu {r['cpu']:5.2f}, render {r['gpu']:5.2f}"
              f" (med {r['gpu_med']:5.2f}), drawdone {r['drawdone']:4.2f} ms; late {r['late']:4.1f}%")
        if r["threads"]:
            print(f"{'':14s}{' '.join(r['threads'].split()[:4])}")

keys = [("fps", "fps", 1), ("work", "work ms", 2), ("cpu", "cpu ms", 2), ("gpu", "render ms", 2), ("drawdone", "drawdone", 2),
        ("sched", "game %", 1), ("render", "render %", 1)]
names = [n for n in rate_of if n in runs] + [n for n in runs if n not in rate_of]   # the variants' order: the first is the base
if names:
    print("\nmedian over runs:" + "".join(f" {label:>10s}" for _, label, _ in keys))
    for n in names:
        print(f"{n:>10s} ({len(runs[n])}):" + "".join(f" {statistics.median(r[k] for r in runs[n].values()):10.{p}f}" for k, _, p in keys))
base = names[0] if names else None
for n in names[1:]:
    common = sorted(set(runs[n]) & set(runs[base]))
    if not common:
        continue
    print(f"\n{n} against {base}, {len(common)} rounds paired:")
    for k, label, _ in keys:
        a = [runs[base][i][k] for i in common]; b = [runs[n][i][k] for i in common]
        diff = [100 * (y - x) / x for x, y in zip(a, b) if x]
        if not diff:
            continue
        below = sum(1 for x, y in zip(a, b) if y < x)
        sep = "every run below every run" if max(b) < min(a) else "every run above every run" if min(b) > max(a) else "overlapping"
        print(f"  {label:>10s}: {statistics.median(a):7.2f} -> {statistics.median(b):7.2f}, paired median {statistics.median(diff):+5.1f}%"
              f" (range {min(diff):+5.1f} .. {max(diff):+5.1f}), lower in {below}/{len(common)} rounds; {sep}")
