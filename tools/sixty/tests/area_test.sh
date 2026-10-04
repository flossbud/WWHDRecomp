#!/usr/bin/env bash
# area_test.sh "STAGE,point,room,layer" FROM TO P,Q,... [ROUTE]: route_test.sh for several types at once,
# after a warp at f920 (WWHD_DEBUG_STAGE; WARP=tick to change it; "-" for no warp): the step-doubling
# trial on all of them over FROM-TO (each type's lines), then 30 against 60 with them all converted on top
# of the defaults: every type's share of ticks matching the 30-tick run (actor_types.py) and each
# instance's path (paths.py). Types that don't touch each other only: one run instead of one per type.
set -e
source "$(dirname "$0")/common.sh"
st=$1; a=$2; b=$3; ps=$4; r=${5:-save}
out=$OUT/area; rm -rf "$out"; mkdir -p "$out"
[ "$st" != - ] && export WWHD_DEBUG_STAGE="${WARP:-920}:$st"
WWHD_60FPS_TRIAL=$ps WWHD_60FPS_TRIAL_TICKS=$a-$b SIXTY_FRAMES=$((b+5)) tools/sixty/run.sh $r $out/trial 30 > /dev/null 2>&1
for p in ${ps//,/ }; do
    echo "== trial ($p)"
    python3 tools/sixty/trial.py $out/trial/30 --target $p --top 800 | trial_filter | head -25
done
WWHD_STATE_TRACK=$ps WWHD_60FPS_CONVERT=$(defaults),$ps SIXTY_FRAMES=$b tools/sixty/run.sh $r $out/cmp 30 60 > /dev/null 2>&1
echo "== 30 against 60"
python3 tools/sixty/actor_types.py $out/cmp/30 $out/cmp/60 --from $a --names /wwhd/data/ghidra-out/actor_names.tsv
python3 tools/sixty/tests/paths.py $out/cmp $a $b
