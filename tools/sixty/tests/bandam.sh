#!/usr/bin/env bash
# bandam.sh: Doc Bandam's cups and pots stand still at 60 (B81, session top). Route bandam after a warp into the shop
# (Pdrgsh, Link by the door), at 30 and at 60 side by side (llvmpipe, about 5 minutes): the shelf with the dishes at
# game frame 1071 (swap 1242 at 60) is the 30-tick run's picture. RMSE of that crop: 0.005 when they match (fog), 0.073
# when they had tumbled (the bug); a FAIL above BANDAM_MAX (0.02). Run it on the worker:
# tools/worker/job start bandam tools/sixty/tests/bandam.sh
set -uo pipefail
source "$(dirname "$0")/common.sh"
W=$OUT/bandam; rm -rf "$W"; mkdir -p "$W/b30" "$W/b60"
cp build/wwhd/wwhd-null "$W/b30/" && cp build/wwhd/wwhd-null "$W/b60/" || exit 2
export WWHD_DEBUG_STAGE=920:Pdrgsh,0,0,-1
CEMU_BIN=$W/b30/wwhd-null SIXTY_OUT=$W/r30 tools/sixty/tests/shots.sh bandam 1071 30 > /dev/null 2>&1 &
CEMU_BIN=$W/b60/wwhd-null SIXTY_OUT=$W/r60 tools/sixty/tests/shots.sh bandam 1242 60 > /dev/null 2>&1 &
wait
a=$W/r30/shots-bandam/f001071.tv.ppm b=$W/r60/shots-bandam/f001242.tv.ppm
[ -f "$a" ] && [ -f "$b" ] || { echo "FAIL  no capture ($a, $b)"; echo "bandam: FAIL"; exit 1; }
c=900x200+500+80                                # the shelf with the dishes, in a 1920x1080 capture
d=$(compare -metric RMSE <(convert "$a" -crop $c ppm:-) <(convert "$b" -crop $c ppm:-) null: 2>&1 | sed 's/.*(\(.*\))/\1/')
if awk -v d="$d" -v m="${BANDAM_MAX:-0.02}" 'BEGIN { exit !(d != "" && d <= m) }'; then
    echo "PASS  the dishes at 60 as at 30 (RMSE $d)"; echo "bandam: ok"
else
    echo "FAIL  the dishes at 60 differ from 30 (RMSE ${d:-none})"; echo "bandam: FAIL"; exit 1
fi
