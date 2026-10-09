#!/usr/bin/env bash
# rt_routes.sh [ROUTE...]: scripted routes at 60 in REAL TIME, twice with one host thread and twice with three
# (WWHD_CORES=3, docs/research/threads.md), and Link's path compared run against run (session cloud; session top made
# it predeploy's routes, WWHD_CORES and parallel). Real time isn't exact, so one host thread against one host thread
# is the noise floor the three-host-thread runs are judged against.
# Each run: tools/sixty/run.sh with SIXTY_REALTIME=1, WWHD_STATE_TRACK=168, the lazy DrawDone; a binary copy per slot
# (its portable folder: one game per folder).
# RT_VARIANTS (default "st1 st2 mt1 mt2"): mt* runs add RT_MT_ARGS (default WWHD_CORES=3), st* runs WWHD_CORES=1.
# RT_RENDER=1: rendered (WWHD_RENDER=vk; default the null GPU, the game's logic only).
# RT_JOBS runs at once (default 4; real time: the desktop worker has the threads, the worker wants 1).
# The routes' first three menu presses come later (WWHD_INPUT_REMAP, RT_REMAP: 420, 540, 660 -> 530, 620, 700; Start
# stays at 780, so play begins on the same frame): in real time the title can take input after frame 420. A run that
# still never reaches play on time (below) is played again, up to RT_RETRIES (3) times.
# Default routes: predeploy.sh's. RT_COMPARE_ONLY=1: compare the runs already there, play nothing.
# Output: game state, it stays on the worker ($OUT/rt/ROUTE/VARIANT/60); each run's guest threads at its exit
# (WWHD_THREAD_DUMP: VARIANT.threads) and the emulator's log (VARIANT.emu.log).
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
    local try
    for try in $(seq 0 "${RT_RETRIES:-3}"); do
        rm -rf "$R/$r/$v"; mkdir -p "$R/$r/$v"
        env ${w:+WWHD_DEBUG_STAGE=920:$w} ${n:+SIXTY_FRAMES=$n} "${extra[@]}" WWHD_THREAD_DUMP="$R/$r/$v.threads" WWHD_INPUT_REMAP=${RT_REMAP-420:530,540:620,660:700} SIXTY_REALTIME=1 ${RT_RENDER:+WWHD_RENDER=vk} WWHD_LAZY_DRAWDONE=1 WWHD_STATE_TRACK=168 \
            CEMU_BIN="$R/bin.$$.$slot/wwhd-null" SIXTY_OUT="$R/slot.$$.$r.$v" tools/sixty/run.sh "$r" "$R/$r/$v" 60 > "$R/$r/$v.log" 2>&1 \
            || echo "rt_routes: $r $v: the run failed (see $R/$r/$v.log)"
        cp "$R/bin.$$.$slot/portable/log.txt" "$R/$r/$v.emu.log" 2>/dev/null   # the emulator's log (a crash's last words)
        rm -rf "$R/slot.$$.$r.$v"
        # Real time and scripted input: the title's first A (frame 420) can come before the title takes input. With
        # three host threads the game swaps at full rate while the title loads on the other cores, so under a busy
        # machine the title is ready a few dozen swaps later than with one (session top: the same runs with the menu
        # presses 150 frames later, 6 of 6 in play). The run then sits on the file select (no process made after the
        # title's) or comes into play late, when a later press of the route's starts the game, and every input after
        # it lands late. In play: a Link (process 168) whose track starts between ticks 800 and 900 (the title's Link
        # is made at ~tick 100, the play scene's at ~tick 847). Otherwise play it again, and count it.
        python3 - "$R/$r/$v/60/track.bin" <<'PY' && break
import sys
sys.path.insert(0, "tools/sixty")
import compare
T = compare.load_track(sys.argv[1])
sys.exit(0 if any(k[0] == 168 and 1600 <= min(T[k]) <= 1800 for k in T if T[k]) else 1)
PY
        echo "rt_routes: $r $v: never in play (the title's first press too early), again" | tee -a "$R/$r/retries.txt"
    done
    echo "rt_routes: $r $v done $(date +%H:%M)"
}
slot=0
[ -n "${RT_COMPARE_ONLY:-}" ] && routes_run=() || routes_run=("${routes[@]}")   # RT_COMPARE_ONLY=1: the last runs compared again
for r in "${routes_run[@]}"; do
    rm -f "$R/$r/retries.txt"
    for v in "${variants[@]}"; do
        run_one "$r" "$v" $((slot % jobs)) &
        slot=$((slot + 1))
        [ $((slot % jobs)) -eq 0 ] && wait
    done
done
wait
rm -rf "$R"/bin.$$.*
for r in "${routes_run[@]}"; do cat "$R/$r/retries.txt" 2>/dev/null; done | awk '{print $3}' | sort | uniq -c |
    sed 's/^ *\([0-9]*\) \(.*\)/rt_routes: \2: \1 runs played again (never in play)/' 
python3 - "$R" "${variants[*]}" "${routes[@]}" <<'PY'
import math, os, struct, sys
sys.path.insert(0, "tools/sixty")
import compare
R, variants, routes = sys.argv[1], sys.argv[2].split(), sys.argv[3:]
def link(path):
    T = compare.load_track(path)
    if not any(k[0] == 168 and 1600 <= min(T[k]) <= 1800 for k in T if T[k]):
        return "late"                                # never in play on time (the title's press): not compared
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
        if A == "late" or B == "late":
            cells.append(f"{'late start':>17s}"); continue
        if not A or not B:
            cells.append(f"{'no track':>17s}"); continue
        # a half tick's key is 2k+1 or 2k-1 by the run (where the 60 switch fell against the game's ticks): the
        # whole ticks' keys (even) the same, the half ticks' paired by the offset Link's path matches best
        def paired(off):
            ks = [k for k in sorted(A) if k >= 1800 and (k + (0 if k % 2 == 0 else off)) in B]
            return ks, [math.dist(pos(A[k]), pos(B[k + (0 if k % 2 == 0 else off)])) for k in ks]
        ks, d = min((paired(o) for o in (0, 2, -2)), key=lambda p: (-len(p[0]), sum(p[1]) / len(p[1]) if p[1] else 0))
        if not ks:
            cells.append(f"{'no common keys':>17s}"); continue
        end = "" if max(A) == max(B) else "*"
        cells.append(f"{d[-1]:8.1f}/{max(d):7.1f}{end}")
    print(f"{r:10s} " + "  ".join(cells))
print("(units apart at the last common key / farthest on the way; * the runs ended on different keys)")
PY
