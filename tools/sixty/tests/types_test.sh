#!/usr/bin/env bash
# types_test.sh "STAGE,point,room,layer"|- FROM TO [ROUTE]: warp there at f920 (WWHD_DEBUG_STAGE; WARP=tick
# to change it; "-" for none, the route's own place), convert every actor but Link (WWHD_60FPS_CONVERT=
# all,-168: what reads Link sees the 30-tick run's Link) and track them all, then rank every type by the
# ticks it matches the 30-tick run over FROM-TO (tools/sixty/actor_types.py): a type at 100% needs no rules
# of its own. The first pass over an area, after room_list.sh; the rest go to area_test.sh.
set -e
source "$(dirname "$0")/common.sh"
st=$1; a=$2; b=$3
out=$OUT/types; rm -rf "$out"; mkdir -p "$out"
[ "$st" != - ] && export WWHD_DEBUG_STAGE="${WARP:-920}:$st"
WWHD_STATE_TRACK=all WWHD_60FPS_CONVERT=all,-168 SIXTY_FRAMES=$b \
    tools/sixty/run.sh ${4:-save} $out 30 60 > /dev/null 2>&1
python3 tools/sixty/actor_types.py $out/30 $out/60 --from $a --names /wwhd/data/ghidra-out/actor_names.tsv
