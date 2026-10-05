#!/usr/bin/env bash
# events_test.sh BASE_BIN NEW_BIN [ROUTE...]: the event routes at 30 and at 60 with two builds, for a change to
# the event stepping (sixty.cpp's edges, StepInEvents) or to an event's rules: per route, the camera's view
# (camera.py) and Link's path (paths.py, process 168) against the 30-tick run, the base build's line, then
# the new build's. Default routes: back door2 talk cuts items warp door (doors, a talk, cutscenes, a chest,
# the Ballad of Gales, a dungeon door). Each route runs once at 30 (with BASE_BIN; 30 is the same with either
# build) and once at 60 with each; give the binaries in folders of their own (a run writes its portable data
# next to its binary). Output: $OUT/events/ROUTE/{base,new}/{30,60} (game state: it stays on the worker).
set -e
source "$(dirname "$0")/common.sh"
base=${1:?usage: events_test.sh BASE_BIN NEW_BIN [ROUTE...]}; new=${2:?usage: events_test.sh BASE_BIN NEW_BIN [ROUTE...]}
shift 2
routes=("$@")
[ ${#routes[@]} -eq 0 ] && routes=(back door2 talk cuts items warp door)
for r in "${routes[@]}"; do
    d=$OUT/events/$r
    mkdir -p "$d"
    WWHD_STATE_TRACK=168,476 CEMU_BIN=$base tools/sixty/run.sh "$r" "$d/base" 30 60 > /dev/null 2>&1
    WWHD_STATE_TRACK=168,476 CEMU_BIN=$new tools/sixty/run.sh "$r" "$d/new" 60 > /dev/null 2>&1
    ln -sfn "$d/base/30" "$d/new/30"
    echo "### $r"
    for v in base new; do
        echo "-- $v"
        python3 tools/sixty/camera.py "$d/$v/30" "$d/$v/60" 2>&1 | grep "^camera" || true
        python3 tools/sixty/tests/paths.py "$d/$v" 900 13000 2>&1 | grep "^(168" || true
    done
done
