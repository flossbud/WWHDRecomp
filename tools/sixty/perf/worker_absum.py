"""worker_absum.py DIR [ROUTE]: worker-ab.sh's runs summarised, gameplay only (game frame 900 on; round 0, the
warm-up, left out). Per run: the fps, the scheduler (game) thread's and the render thread's CPU (% of a core, from
the frame log), the game thread's work a frame (wall, to its wait: GX2DrawDone's wait included) and CPU a frame, the
render thread's CPU a frame (mean and median, ms), the DrawDone wait, late frames, and the threads' CPU (.threads);
with three host threads (WWHD_CORES=3) also cores 0 and 2's host threads (% of a core, the frame log's last columns).
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
        fr.append(dict(half=p[1] == "h", dropped=p[1] == "d", began=float(p[2]), work=float(p[3]), drawdone=float(p[4]), cpu=float(p[5]),
                       gpu=float(p[7]), vsyncs=int(p[8]), wait=float(p[9]),
                       core0=float(p[13]) if len(p) > 14 else 0.0, core2=float(p[14]) if len(p) > 14 else 0.0))
    if len(fr) < 10:
        return None
    span = (fr[-1]["began"] + fr[-1]["work"] + fr[-1]["wait"] - fr[0]["began"]) / 1000
    ticks = sum(1 for f in fr if not f["half"] and not f["dropped"]) if rate == 60 else len(fr)
    shown = [f for f in fr if not f["dropped"]]
    m = lambda k: statistics.mean(f[k] for f in shown)
    r = dict(fps=len(shown) / span, speed=100 * ticks / span / 30, sched=sum(f["cpu"] for f in fr) / span / 10, render=sum(f["gpu"] for f in fr) / span / 10,
             core0=sum(f["core0"] for f in fr) / span / 10, core2=sum(f["core2"] for f in fr) / span / 10,
             work=m("work"), cpu=m("cpu"), gpu=m("gpu"), drawdone=m("drawdone"),
             work_med=statistics.median(f["work"] for f in fr), gpu_med=statistics.median(f["gpu"] for f in fr),
             late=100 * sum(1 for f in fr if f["vsyncs"] > 0) / len(fr), n=len(fr))
    stem = path[:-len(".frames")]
    r["threads"] = open(stem + ".threads").read().strip() if os.path.exists(stem + ".threads") else ""
    r["clock"] = clock(stem)
    r["gputime"], r["gpukinds"] = gpu_timing(stem, 1800 if rate == 60 else 900)
    return r

def clock(stem):
    """the GPU's mean clock (MHz) over the gameplay window, or nan"""
    try:
        ta, tb = map(float, open(stem + ".window").read().split())
        v = [float(p[1]) for p in (l.split() for l in open(stem + ".clock")) if len(p) == 2 and ta <= float(p[0]) <= tb]
        return statistics.mean(v) if v else float("nan")
    except (OSError, ValueError):
        return float("nan")

def gpu_timing(stem, start):
    """WWHD_GPU_TIMING's lines ("gpu timing: frames A-B: T ms a frame: kind ms ...") that end after gameplay begins (presented frame
    `start`), weighted by their frames: the total and each kind, ms a frame"""
    tot, kinds, n = 0.0, {}, 0
    try:
        for line in open(stem + ".log", errors="replace"):
            m = re.search(r"gpu timing: frames (\d+)-(\d+): ([\d.]+) ms a frame:(.*)", line)
            if not m or int(m.group(2)) <= start:
                continue
            f = int(m.group(2)) - int(m.group(1)) + 1
            tot += float(m.group(3)) * f; n += f
            p = m.group(4).split()
            for k, v in zip(p[::2], p[1::2]):
                kinds[k] = kinds.get(k, 0.0) + float(v) * f
    except OSError:
        pass
    if not n:
        return float("nan"), {}
    return tot / n, {k: v / n for k, v in kinds.items()}

runs = {}   # name -> {round: stats}
for path in sorted(glob.glob(os.path.join(d, f"ab-*-{route}-*.frames"))):
    # ab-NAME-ROUTE-N.frames; the route may hold hyphens (en-tn): with ROUTE given, split on it
    base = os.path.basename(path)
    m = (re.match(rf"ab-(.+)-({re.escape(route)})-(\d+)\.frames$", base) if route != "*" else None) \
        or re.match(r"ab-(.+?)-(.+)-(\d+)\.frames$", base)
    if not m or m.group(3) == "0":
        continue
    name = m.group(1)
    r = run(path, rate_of.get(name, 60))
    if r:
        runs.setdefault(name, {})[int(m.group(3))] = r
        print(f"{name:>10s} {m.group(3):>2s}: {r['fps']:5.1f} fps, speed {r['speed']:3.0f}%, game thread {r['sched']:5.1f}%, render {r['render']:5.1f}%;"
              f" a frame: work {r['work']:5.2f} (med {r['work_med']:5.2f}), cpu {r['cpu']:5.2f}, render {r['gpu']:5.2f}"
              f" (med {r['gpu_med']:5.2f}), drawdone {r['drawdone']:4.2f} ms; late {r['late']:4.1f}%"
              + (f"; cores 0/2 {r['core0']:4.1f}/{r['core2']:4.1f}%" if r['core0'] or r['core2'] else ""))
        if r["threads"]:
            print(f"{'':14s}{' '.join(r['threads'].split()[:4])}")
        if r["clock"] == r["clock"] or r["gpukinds"]:
            print(f"{'':14s}GPU clock {r['clock']:.0f} MHz; GPU {r['gputime']:.2f} ms a frame: "
                  + " ".join(f"{k} {v:.2f}" for k, v in sorted(r["gpukinds"].items(), key=lambda kv: -kv[1])))

keys = [("fps", "fps", 1), ("speed", "speed %", 0), ("work", "work ms", 2), ("cpu", "cpu ms", 2), ("gpu", "render ms", 2), ("drawdone", "drawdone", 2),
        ("sched", "game %", 1), ("render", "render %", 1), ("clock", "GPU MHz", 0), ("gputime", "GPU ms", 2), ("core0", "core0 %", 1),
        ("core2", "core2 %", 1)]
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
