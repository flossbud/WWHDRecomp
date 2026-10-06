"""Link's action changes at 30 against 60, per route of a predeploy run (session top, animstart). Usage (on
the worker; it reads the runs' tracks):
    python3 tools/sixty/acttiming.py $SIXTY_OUT/predeploy
For each route DIR/<route>/{30,60}/track.bin, paired as predeploy pairs them: how many of 30's action
changes (Link +0x65F0) come in the same tick at 60 (its whole or half step), how many a tick or more apart
(the first few, action@30's tick+offset), and where the two sequences part; then the totals."""
import sys, os, struct, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import compare
def link(p):
    best = None
    for k, v in compare.load_track(p).items():
        if k[0] == 168 and (best is None or max(v) > max(best)): best = v
    return best
act = lambda v, k: struct.unpack(">I", v[k][0x65F0:0x65F4])[0]
pos = lambda v, k: struct.unpack(">3f", v[k][0x314:0x320])
tot = [0, 0, 0]
for r in sorted(os.listdir(sys.argv[1])):
    pa, pb = (os.path.join(sys.argv[1], r, x, "track.bin") for x in ("30", "60"))
    if not (os.path.exists(pa) and os.path.exists(pb)): continue
    a, b = link(pa), link(pb)
    def paired(off):
        ts = [t for t in sorted(a) if t % 2 == 0 and t + off in b and t >= 1800]
        return ts, [math.dist(pos(a, t), pos(b, t + off)) for t in ts]
    off = min((1, -1), key=lambda o: (lambda ts, d: sum(d) / len(d) if d else float("inf"))(*paired(o)))
    ka = [t for t in sorted(a) if t % 2 == 0 and t >= 1800]
    ca = [(t // 2, act(a, t)) for i, t in enumerate(ka) if i and act(a, t) != act(a, ka[i - 1])]
    kb = [t for t in sorted(b) if t >= 1800]
    cb = [((t - off) / 2, act(b, t)) for i, t in enumerate(kb) if i and act(b, t) != act(b, kb[i - 1])]
    same = apart = 0; part = None; offs = []
    for i in range(min(len(ca), len(cb))):
        if ca[i][1] != cb[i][1]:
            part = "%02x at 30's %d, %02x at 60's %.1f" % (ca[i][1], ca[i][0], cb[i][1], cb[i][0]); break
        d = cb[i][0] - ca[i][0]
        if abs(d) < 0.6: same += 1
        else: apart += 1; offs.append("%02x@%d%+.1f" % (ca[i][1], ca[i][0], d))
    else:
        if len(ca) != len(cb): part = "30 has %d changes, 60 %d" % (len(ca), len(cb))
    tot[0] += same; tot[1] += apart; tot[2] += part is not None
    print("%-7s same tick %3d, apart %2d %s%s" % (r, same, apart, " ".join(offs[:4]), ("; parts: " + part) if part else ""))
print("all: same tick %d, apart %d, routes parting %d" % tuple(tot))
