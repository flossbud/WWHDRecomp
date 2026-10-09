#!/usr/bin/env bash
# rt_routes.sh [ROUTE...]: scripted routes at 60 in REAL TIME, twice with one host thread and twice with three
# (WWHD_CORES=3, docs/research/threads.md), and Link's path compared run against run (session cloud; session top made
# it predeploy's routes, WWHD_CORES and parallel). Real time isn't exact, so one host thread against one host thread
# is the noise floor the three-host-thread runs are judged against.
# Each run: tools/sixty/run.sh with SIXTY_REALTIME=1, WWHD_STATE_TRACK=168, the lazy DrawDone; a binary copy per slot
# (its portable folder: one game per folder).
# RT_VARIANTS (default "st1 st2 mt1 mt2"): mt* runs add RT_MT_ARGS (default WWHD_CORES=3), st* runs WWHD_CORES=1.
# RT_JOBS runs at once (default 4; real time: the desktop worker has the threads, the worker wants 1).
# Default routes: predeploy.sh's.
# Output: game state, it stays on the worker ($OUT/rt/ROUTE/VARIANT/60).
set -uo pipefail
source "$(dirname "$0")/common.sh"
routes=("$@"); [ ${#routes[@]} -eq 0 ] && routes=(warp back door house talk items cuts leaf hook ladder crawl carry spin bow shield swing land3 sidle2 pot plants slash sail menus tour3 boomerang bombs grapple boots hammer armor mirror heavy drcjar fwbud tgbeam wtspring medli gtrock en-rd en-ph en-pz en-bo tgstatue medliharp gtgrapple fwswitch crate slope warppot climb)
variants=(${RT_VARIANTS:-st1 st2 mt1 mt2})
jobs=${RT_JOBS:-4}
R=$OUT/rt; mkdir -p "$R"
for i in $(seq 0 $((jobs - 1))); do
    mkdir -p "$R/bin.$$.$i" && cp build/wwhd/wwhd-null "$R/bin.$$.$i/wwhd-null" || exit 2
done
run_one() {
    local r=$1 v=$2 slot=$3 w= n= extra=()
    case $r in back) w=M_NewD2,5,14,-1 ;; talk) w=sea,0,11,-1 ;; house) w=sea,9,11,-1 ;; warp) n=2460 ;; esac
    if [[ $v == mt* ]]; then extra=(${RT_MT_ARGS:-WWHD_CORES=3}); else extra=(WWHD_CORES=1); fi
    rm -rf "$R/$r/$v"; mkdir -p "$R/$r/$v"
    env ${w:+WWHD_DEBUG_STAGE=920:$w} ${n:+SIXTY_FRAMES=$n} "${extra[@]}" SIXTY_REALTIME=1 WWHD_LAZY_DRAWDONE=1 WWHD_STATE_TRACK=168 \
        CEMU_BIN="$R/bin.$$.$slot/wwhd-null" SIXTY_OUT="$R/slot.$$.$r.$v" tools/sixty/run.sh "$r" "$R/$r/$v" 60 > "$R/$r/$v.log" 2>&1 \
        || echo "rt_routes: $r $v: the run failed (see $R/$r/$v.log)"
    rm -rf "$R/slot.$$.$r.$v"
    echo "rt_routes: $r $v done $(date +%H:%M)"
}
slot=0
for r in "${routes[@]}"; do
    for v in "${variants[@]}"; do
        run_one "$r" "$v" $((slot % jobs)) &
        slot=$((slot + 1))
        [ $((slot % jobs)) -eq 0 ] && wait
    done
done
wait
rm -rf "$R"/bin.$$.*
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
