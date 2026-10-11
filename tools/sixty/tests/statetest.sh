#!/usr/bin/env bash
# statetest.sh ROUTE SWAP [RATE] (q-savestate, session qa): a save state carries the whole game. The route plays at RATE
# (default 60) on the virtual clock twice: A saves at SWAP (WWHD_STATE_SAVE) and plays on; B boots anew, loads that state
# at its first save point (WWHD_STATE_LOAD) and plays on from there. From the state's swap to the route's end every whole
# tick's actors (WWHD_STATE_DUMP's hashes) must be the same in both: FAIL on any that differs, on a save or load that
# didn't happen, or on a run that ended early. STATE_INPLACE=SWAP2: B plays the route to SWAP2 and loads there, in place
# (WWHD_STATE_LOAD_AT: the debug menu's way). STATE_FRAMES: the game frame the runs end at (default the route's).
# STATE_KEEP=1 keeps the state file. Run it as a job: tools/worker/job start state tools/sixty/tests/statetest.sh sail 1500
# STATE_RT=1: in real time instead (wwhd-play, or STATE_RT_BIN; three host threads, the lazy DrawDone, the null GPU), one
# run that saves at SWAP and loads in place at STATE_INPLACE (default SWAP + 600: ten seconds later, as a player's load
# comes): FAIL on a save or load that didn't happen, fewer than 300 frames after the load, or a frame after it taking
# over STATE_RT_MAX_MS (1000; B82: the pacing waited out the time between save and load, 68 s on the Deck)
set -uo pipefail
cd "$(dirname "$0")/../../.."
source tools/sixty/tests/common.sh
route=${1:?route} swap=${2:?swap} rate=${3:-60}
W=$OUT/statetest-$route-$swap-$rate${STATE_RT:+-rt}; rm -rf "$W"; mkdir -p "$W/binA" "$W/binB"
if [ -n "${STATE_RT:-}" ]; then
    inplace=${STATE_INPLACE:-$((swap + 600))}
    cp "${STATE_RT_BIN:-build/wwhd/wwhd-play}" "$W/binA/" || exit 2
    env ${STATE_FRAMES:+SIXTY_FRAMES=$STATE_FRAMES} SIXTY_REALTIME=1 WWHD_CORES=3 WWHD_LAZY_DRAWDONE=1 WWHD_STATE_SAVE="$swap:$W/state" \
        WWHD_STATE_LOAD_AT="$inplace:$W/state" WWHD_FRAME_LOG="$W/frames.txt" CEMU_BIN="$W/binA/$(basename "${STATE_RT_BIN:-wwhd-play}")" \
        tools/sixty/run.sh "$route" "$W/A" "$rate" > "$W/A.log" 2>&1
    grep -a "wwhd state:" "$W"/binA/portable/log.txt | sed 's/^.*\] //'
    [ -n "${STATE_KEEP:-}" ] || rm -f "$W/state"
    python3 - "$W" "$inplace" "${STATE_RT_MAX_MS:-1000}" <<'PY2'
import os, sys
W, inplace, limit = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
log = open(os.path.join(W, "binA", "portable", "log.txt"), errors="replace").read()
rows = []
if os.path.exists(os.path.join(W, "frames.txt")):
    rows = [(int(f[0]), float(f[2])) for f in (l.split() for l in open(os.path.join(W, "frames.txt")) if not l.startswith("#"))]
# the load: the first frame whose swap is below the one before (in place, back to the state's)
at = next((i for i in range(1, len(rows)) if rows[i][0] < rows[i - 1][0]), None)
if "wwhd state: saved" not in log or "wwhd state: loaded" not in log or at is None:
    print(f"FAIL  statetest (real time): the save or the load in place at {inplace} didn't happen ({W})")
    sys.exit(1)
after = rows[at:]                                  # (the first one's time holds the load's own)
times = [(after[i + 1][1] - after[i][1], after[i][0]) for i in range(1, len(after) - 1)]
worst = max(times) if times else (0.0, 0)
bad = len(after) < 300 or worst[0] > limit
print(f"{'FAIL' if bad else 'ok':5s} statetest (real time): loaded in place at swap {rows[at - 1][0]}, back to {rows[at][0]}; "
      f"{len(after)} frames after, the longest {worst[0]:.0f} ms (swap {worst[1]}; limit {limit:.0f})")
sys.exit(1 if bad else 0)
PY2
    exit $?
fi
cp build/wwhd/wwhd-null "$W/binA/" && cp build/wwhd/wwhd-null "$W/binB/" || exit 2
frames=(${STATE_FRAMES:+SIXTY_FRAMES=$STATE_FRAMES})
env "${frames[@]}" WWHD_STATE_SAVE="$swap:$W/state" CEMU_BIN="$W/binA/wwhd-null" tools/sixty/run.sh "$route" "$W/A" "$rate" > "$W/A.log" 2>&1
load=(WWHD_STATE_LOAD="$W/state"); [ -n "${STATE_INPLACE:-}" ] && load=(WWHD_STATE_LOAD_AT="$STATE_INPLACE:$W/state")
env "${frames[@]}" "${load[@]}" CEMU_BIN="$W/binB/wwhd-null" tools/sixty/run.sh "$route" "$W/B" "$rate" > "$W/B.log" 2>&1
grep -a "wwhd state:" "$W/binA/portable/log.txt" "$W/binB/portable/log.txt" | sed 's/^.*\] //'
[ -n "${STATE_KEEP:-}" ] || rm -f "$W/state"
python3 - "$W" "$rate" "$swap" <<'PY'
import collections, os, sys
W, rate, swap = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
first = swap if rate == 30 else 900 + (swap - 900) // 2      # the game frame of the state's swap
def load(run):
    d = collections.defaultdict(dict)
    p = os.path.join(W, run, str(rate), "hashes.txt")
    if not os.path.exists(p):
        return None
    for l in open(p):
        f = l.split()
        if len(f) >= 5 and int(f[0]) >= first:
            d[int(f[0])][(f[1], f[2])] = f[4]
    return d
A, B = load("A"), load("B")
log = lambda run: open(os.path.join(W, "bin" + run, "portable", "log.txt"), errors="replace").read()
if A is None or B is None or "wwhd state: saved" not in log("A") or "wwhd state: loaded" not in log("B"):
    print(f"FAIL  statetest: the save or the load didn't happen ({W})")
    sys.exit(1)
ticks = sorted(set(A) | set(B))
bad = [t for t in ticks if A.get(t) != B.get(t)]
short = len(A) != len(B)
for t in bad[:8]:
    a, b = A.get(t, {}), B.get(t, {})
    diff = sorted(k for k in set(a) | set(b) if a.get(k) != b.get(k))
    print(f"      tick {t}: {len(diff)} of {len(set(a) | set(b))} actors differ, e.g. {diff[:4]}")
verdict = "FAIL" if bad or short or not ticks else "ok"
print(f"{verdict:5s} statetest: {len(ticks)} whole ticks after the state (game frames {first}..{ticks[-1] if ticks else '-'}), "
      f"{len(bad)} differ{'; A and B ended apart' if short else ''}")
sys.exit(1 if verdict == "FAIL" else 0)
PY
