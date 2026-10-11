#!/usr/bin/env bash
# blacktest.sh [ROUTE]: no black pictures from dropped frames (q-deckblack, session qa; predeploy.sh runs it). The route
# (default slash) on the virtual clock, rendered, with the present probe (WWHD_PRESENT_PROBE_OUT, present.cpp: for every
# present the game's swap it shows and whether its scan buffer is black), once plain and once for each way the pacing
# drops frames: keep-speed's half frames (WWHD_60FPS_DROPTEST=2), the 40 cap (WWHD_FPS_CAP=40) and the frame skip's whole
# frames at 60 and at 30 (WWHD_FRAMESKIP_TEST=2). FAIL: a present black where the plain run's at the same swap isn't
# (the frame after a dropped one drew from lists never built and showed black: one black frame for every dropped one,
# the Steam Deck's flicker, which droptest.sh's game state and the fps figures never showed), a run with no presents, or
# one whose drops took no present (nothing tested). BLACK_FRAMES: the game frame the runs end at (default 1250: play at 60
# from swap 900, so 700 swaps of it). BLACK_GPU: REF_GPU (default the worker's own GPU: the pictures needn't be exact).
# BLACK_JOBS runs at once (default 3). Run it as a job: tools/worker/job start black tools/sixty/tests/blacktest.sh
set -uo pipefail
cd "$(dirname "$0")/../../.."
source tools/sixty/tests/common.sh
route=${1:-slash}
B=$OUT/blacktest; [ -z "${BLACK_ONLY:-}" ] && rm -rf "$B"; mkdir -p "$B"
variants=("b60 60" "d60 60 WWHD_60FPS_DROPTEST=2" "c60 60 WWHD_FPS_CAP=40" "s60 60 WWHD_FRAMESKIP_TEST=2" "b30 30" "s30 30 WWHD_FRAMESKIP_TEST=2")
run() {
    local name=$1 rate=$2; shift 2
    mkdir -p "$B/bin-$name" && cp build/wwhd/wwhd-null "$B/bin-$name/" || return
    env "$@" ${BLACK_GPU:+REF_GPU=$BLACK_GPU} WWHD_RENDER=vk WWHD_PRESENT_PROBE=1 WWHD_PRESENT_PROBE_OUT="$B/$name.presents" \
        SIXTY_FRAMES=${BLACK_FRAMES:-1250} CEMU_BIN="$B/bin-$name/wwhd-null" tools/sixty/run.sh "$route" "$B/run-$name" "$rate" > "$B/$name.log" 2>&1
    cp "$B/bin-$name/portable/log.txt" "$B/$name.emu.log" 2>/dev/null
    rm -rf "$B/run-$name" "$B/bin-$name"                    # game state dumps and the binary: not needed after
}
n=0
for v in "${variants[@]}"; do
    [ -n "${BLACK_ONLY:-}" ] && [[ " $BLACK_ONLY " != *" ${v%% *} "* ]] && continue   # BLACK_ONLY="b30 s30": those runs only
    run $v &
    n=$((n + 1)); [ $((n % ${BLACK_JOBS:-3})) = 0 ] && wait
done
wait
python3 - "$B" "$route" <<'PY'
import os, sys
B, route = sys.argv[1], sys.argv[2]
def load(name):
    d = {}
    try:
        for line in open(os.path.join(B, name + ".presents")):
            s, _, black = line.split()
            if int(s):
                d[int(s)] = int(black)
    except (OSError, ValueError):
        return None
    return d
bad = 0
for name, base, what in (("d60", "b60", "keep-speed's half frames dropped (every 2nd)"), ("c60", "b60", "the 40 cap"),
                         ("s60", "b60", "the frame skip at 60 (every 2nd whole frame)"), ("s30", "b30", "the frame skip at 30 (every 2nd frame)")):
    V, P = load(name), load(base)
    if not V or not P:
        print(f"FAIL  {name}: no presents logged ({name if not V else base}; see {B}/{name}.log)"); bad += 1; continue
    lo = min(V)
    common = [s for s in V if s in P]
    black = sorted(s for s in common if V[s] and not P[s])
    taken = sum(1 for s in P if s not in V and lo <= s <= max(V))
    stray = sum(1 for s in V if s not in P)
    verdict = "FAIL" if black or not taken else "ok"
    bad += verdict == "FAIL"
    print(f"{verdict:5s} {name}: {what}: {len(V)} presents, {taken} taken by the drops, {len(black)} black where the plain run's aren't"
          + (f" (swaps {', '.join(map(str, black[:12]))}{' ...' if len(black) > 12 else ''})" if black else "")
          + (f"; {stray} swaps the plain run never presented" if stray else "") + ("; the drops took no present: nothing tested" if not taken else ""))
print(f"blacktest {route}: {'FAIL' if bad else 'ok'} ({B})")
sys.exit(1 if bad else 0)
PY
