#!/usr/bin/env bash
# spawn_test.sh PROC [PARAM] [Y]: spawn PROC on the Outset dock behind Link at f950 (route save,
# WWHD_DEBUG_SPAWN), the step-doubling trial on it over f951-1240, then 30 against 60 with it converted
# (its position: 30's tick k against 60's half frame k; D21). On the worker: tools/worker/job start ...
set -e
source "$(dirname "$0")/common.sh"
p=$1; prm=${2:-0}; y=${3:-190}
spawn="950:$p,$prm,-201622,$y,312700"
out=$OUT/spawn-$p; rm -rf "$out"; mkdir -p "$out"
WWHD_DEBUG_SPAWN="$spawn" WWHD_60FPS_TRIAL=$p WWHD_60FPS_TRIAL_TICKS=951-1240 SIXTY_FRAMES=1245 tools/sixty/run.sh save $out/trial 30 > /dev/null 2>&1
echo "== trial ($p)"
python3 tools/sixty/trial.py $out/trial/30 --target $p --top 600 | trial_filter | head -30
WWHD_DEBUG_SPAWN="$spawn" WWHD_STATE_TRACK=$p WWHD_60FPS_CONVERT=$(defaults),$p SIXTY_FRAMES=1240 tools/sixty/run.sh save $out/cmp 30 60 > /dev/null 2>&1
echo "== 30 against 60"
python3 tools/sixty/tests/paths.py $out/cmp 951 1240
