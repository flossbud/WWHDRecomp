#!/usr/bin/env bash
# stage_test.sh PROC "STAGE,point,room,layer" FROM TO [SPAWN]: warp at f920 (WWHD_DEBUG_STAGE; WARP=tick
# to change it, ROUTE= another route than save), optionally spawn (WWHD_DEBUG_SPAWN format), then the
# trial on PROC over FROM-TO and 30 against 60 with it converted (on top of base: common.sh). Bosses on a
# finished save: WWHD_DEBUG_BOSS=1. Rooms: warp and list the processes (see docs/handoff.md).
set -e
source "$(dirname "$0")/common.sh"
p=$1; st=$2; a=$3; b=$4; sp=${5:-}
out=$OUT/stage-$p; rm -rf "$out"; mkdir -p "$out"
export WWHD_DEBUG_STAGE="${WARP:-920}:$st"
[ -n "$sp" ] && export WWHD_DEBUG_SPAWN="$sp"
WWHD_60FPS_TRIAL=$p WWHD_60FPS_TRIAL_TICKS=$a-$b SIXTY_FRAMES=$((b+5)) tools/sixty/run.sh ${ROUTE:-save} $out/trial 30 > /dev/null 2>&1
echo "== trial ($p)"
python3 tools/sixty/trial.py $out/trial/30 --target $p --top 600 | trial_filter | head -40
WWHD_STATE_TRACK=$p WWHD_60FPS_CONVERT=$(base),$p SIXTY_FRAMES=$b tools/sixty/run.sh ${ROUTE:-save} $out/cmp 30 60 > /dev/null 2>&1
echo "== 30 against 60"
python3 tools/sixty/tests/paths.py $out/cmp $a $b
