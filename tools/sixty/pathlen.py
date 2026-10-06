"""Link's path length (the distance he covers, whatever his heading) at 30 against 60, per route of a predeploy
run (session top, the ramp item). Usage (on the worker; it reads the runs' tracks):
    python3 tools/sixty/pathlen.py $SIXTY_OUT/predeploy
For each route DIR/<route>/{30,60}/track.bin: the path at 30 and at 60 (30's tick k paired with the 60 run's
half tick that holds its state, as predeploy pairs them), the final difference and the largest on the way.
A heading off by a little (the camera's) moves Link elsewhere but the same distance, so this shows a speed
that differs (the walk start's ramp) where predeploy's distances show the heading too. Steps over 40 units
(a warp) are left out."""
import sys, os, struct, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import compare
def link(p):
    best = None
    for k, v in compare.load_track(p).items():
        if k[0] == 168 and (best is None or max(v) > max(best)): best = v
    return best
pos = lambda v, k: struct.unpack(">3f", v[k][0x314:0x320])
tot = [0.0, 0.0]
for r in sorted(os.listdir(sys.argv[1])):
    pa, pb = (os.path.join(sys.argv[1], r, x, "track.bin") for x in ("30", "60"))
    if not (os.path.exists(pa) and os.path.exists(pb)): continue
    a, b = link(pa), link(pb)
    def paired(off):
        ts = [t for t in sorted(a) if t % 2 == 0 and t + off in b and t >= 1800]
        return ts, [math.dist(pos(a, t), pos(b, t + off)) for t in ts]
    off = min((1, -1), key=lambda o: (lambda ts, d: sum(d) / len(d) if d else float("inf"))(*paired(o)))
    ts = [t for t in sorted(a) if t % 2 == 0 and t + off in b and t >= 1800]
    la = lb = 0.0; worst = 0.0; diffs = []
    for i in range(1, len(ts)):
        t0, t1 = ts[i - 1], ts[i]
        da = math.dist(pos(a, t1), pos(a, t0)); db = math.dist(pos(b, t1 + off), pos(b, t0 + off))
        if da > 40 or db > 40: continue                # a warp or a teleport: left out
        la += da; lb += db
        d = lb - la
        worst = max(worst, abs(d)); diffs.append(abs(d))
    tot[0] += abs(lb - la); tot[1] += worst
    print("%-7s path 30 %8.1f, 60 %8.1f: final %+6.1f, largest %5.1f" % (r, la, lb, lb - la, worst))
print("sums: |final| %.1f, largest %.1f" % tuple(tot))
