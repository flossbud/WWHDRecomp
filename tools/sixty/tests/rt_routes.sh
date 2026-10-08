#!/usr/bin/env bash
# rt_routes.sh [ROUTE...]: scripted routes at 60 in REAL TIME, twice with one host thread and twice with Cemu's three
# (docs/research/threads.md), and Link's path compared run against run (session cloud). Real time isn't exact, so
# one-host-thread against one-host-thread is the noise floor the three-host-thread runs are judged against.
# Each run: tools/sixty/run.sh with SIXTY_REALTIME=1, WWHD_STATE_TRACK=168, the lazy DrawDone; a binary copy each.
# RT_VARIANTS (default "st1 st2 mt1 mt2"): mt* runs add RT_MT_ARGS (default REF_ARGS=--force-multicore-interpreter).
# Output: game state, it stays on the worker ($OUT/rt/ROUTE/VARIANT/60).
set -uo pipefail
source "$(dirname "$0")/common.sh"
routes=("$@"); [ ${#routes[@]} -eq 0 ] && routes=(door house talk items leaf hook ladder carry spin bow swing land3 sail tour3 boomerang grapple en-rd en-bo)
variants=(${RT_VARIANTS:-st1 st2 mt1 mt2})
R=$OUT/rt; mkdir -p "$R/bin"
cp build/wwhd/wwhd-null "$R/bin/wwhd-null.new" && mv "$R/bin/wwhd-null.new" "$R/bin/wwhd-null"
for r in "${routes[@]}"; do
    w=; case $r in back) w=M_NewD2,5,14,-1 ;; talk) w=sea,0,11,-1 ;; house) w=sea,9,11,-1 ;; esac
    for v in "${variants[@]}"; do
        extra=(); [[ $v == mt* ]] && extra=(${RT_MT_ARGS:-REF_ARGS=--force-multicore-interpreter})
        rm -rf "$R/$r/$v"; mkdir -p "$R/$r/$v"
        env ${w:+WWHD_DEBUG_STAGE=920:$w} "${extra[@]}" SIXTY_REALTIME=1 WWHD_LAZY_DRAWDONE=1 WWHD_STATE_TRACK=168 \
            CEMU_BIN="$R/bin/wwhd-null" tools/sixty/run.sh "$r" "$R/$r/$v" 60 > "$R/$r/$v.log" 2>&1 \
            || echo "rt_routes: $r $v: the run failed (see $R/$r/$v.log)"
        echo "rt_routes: $r $v done $(date +%H:%M)"
    done
done
python3 - "$R" "${variants[*]}" "${routes[@]}" <<'PY'
import math, os, struct, sys
sys.path.insert(0, "tools/sixty")
import compare
R, variants, routes = sys.argv[1], sys.argv[2].split(), sys.argv[3:]
def link(path):
    T = compare.load_track(path)
    out = {}
    for k in T:
        if k[0] == 168:
            out.update(T[k])
    return out
pos = lambda b: struct.unpack(">3f", b[0x314:0x320])
pairs = [(a, b) for i, a in enumerate(variants) for b in variants[i + 1:]]
print("route      " + "  ".join(f"{a}-{b:>3s} end/max" for a, b in pairs))
for r in routes:
    t = {}
    for v in variants:
        try:
            t[v] = link(os.path.join(R, r, v, "60", "track.bin"))
        except OSError:
            t[v] = None
    cells = []
    for a, b in pairs:
        A, B = t[a], t[b]
        if not A or not B:
            cells.append(f"{'no track':>17s}"); continue
        ks = [k for k in sorted(A) if k in B and k >= 1800]
        if not ks:
            cells.append(f"{'no common keys':>17s}"); continue
        d = [math.dist(pos(A[k]), pos(B[k])) for k in ks]
        end = "" if max(A) == max(B) else "*"
        cells.append(f"{d[-1]:8.1f}/{max(d):7.1f}{end}")
    print(f"{r:10s} " + "  ".join(cells))
print("(units apart at the last common key / farthest on the way; * the runs ended on different keys)")
PY
