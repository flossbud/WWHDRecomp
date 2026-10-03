#!/usr/bin/env bash
# route_test.sh ROUTE PROC FROM TO: the trial on PROC over FROM-TO of a route (tools/reference/routes.sh;
# WWHD_DEBUG_STAGE etc. pass through), then 30 against 60 with it converted on top of the defaults:
# its path and compare.py --track's summary (the fields that differ most)
set -e
source "$(dirname "$0")/common.sh"
r=$1; p=$2; a=$3; b=$4
out=$OUT/route-$p; rm -rf "$out"; mkdir -p "$out"
WWHD_60FPS_TRIAL=$p WWHD_60FPS_TRIAL_TICKS=$a-$b SIXTY_FRAMES=$((b+5)) tools/sixty/run.sh $r $out/trial 30 > /dev/null 2>&1
echo "== trial ($p)"
python3 tools/sixty/trial.py $out/trial/30 --target $p --top 800 | trial_filter | head -40
WWHD_STATE_TRACK=$p WWHD_60FPS_CONVERT=$(defaults),$p SIXTY_FRAMES=$b tools/sixty/run.sh $r $out/cmp 30 60 > /dev/null 2>&1
echo "== 30 against 60"
python3 tools/sixty/tests/paths.py $out/cmp $a $b
python3 tools/sixty/compare.py $out/cmp/30 $out/cmp/60 --track --from $a 2>&1 | grep -A3 "^  $p at" | head -20
