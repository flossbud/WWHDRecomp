#!/usr/bin/env bash
# trial_n.sh ROUTE FROM TO [N] (U2, session top): the step-doubling trial of every converted process (the defaults) on
# ROUTE over game frames FROM-TO, at 2 steps a tick and at N (WWHD_STEPS, 3 by default), side by side; then the stores
# that differ from the game's step at N but not at 2, by instruction (what the rules at N leave open), and those at
# both (today's, for scale). WARP=STAGE,point,room,layer warps there at f920 first. Run it on the worker:
# tools/worker/job start trialn tools/sixty/tests/trial_n.sh tour3 1000 1300
set -uo pipefail
source "$(dirname "$0")/common.sh"
r=${1:?route} a=${2:?from} b=${3:?to} n=${4:-3}
W=$OUT/trial_n-$r-$n; rm -rf "$W"; mkdir -p "$W"
[ -n "${WARP:-}" ] && export WWHD_DEBUG_STAGE="920:$WARP"
ps=$(defaults)
for s in 2 "$n"; do
    mkdir -p "$W/bin$s" && cp build/wwhd/wwhd-null "$W/bin$s/" || exit 2
    WWHD_STEPS=$s WWHD_60FPS_TRIAL=$ps WWHD_60FPS_TRIAL_TICKS=$a-$b SIXTY_FRAMES=$((b + 5)) CEMU_BIN=$W/bin$s/wwhd-null \
        tools/sixty/run.sh "$r" "$W/run$s" 30 > "$W/run$s.log" 2>&1 &
done
wait
for s in 2 "$n"; do python3 tools/sixty/trial.py "$W/run$s/30" --top 100000 > "$W/trial$s.txt"; done
python3 - "$W/trial2.txt" "$W/trial$n.txt" "$n" <<'PY'
import re, sys
def load(path):
    out = {}
    for line in open(path):
        m = re.match(r"\s*(\d+)\s+[\d.]+%\s+(\S+)\s+(.*)", line)
        if m and "+0x" in m.group(2):
            out[m.group(2)] = (int(m.group(1)), m.group(3)[:110])
    return out
two, at_n, n = load(sys.argv[1]), load(sys.argv[2]), sys.argv[3]
new = sorted((v[0], k, v[1]) for k, v in at_n.items() if k not in two)
print(f"trial_n: {len(two)} instructions differ at 2 steps, {len(at_n)} at {n}; {len(new)} only at {n}:")
for c, k, w in reversed(new):
    print(f"  {c:6d}  {k:28s} {w}")
PY
