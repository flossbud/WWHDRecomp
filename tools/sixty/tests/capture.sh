#!/usr/bin/env bash
# capture.sh NAME "STAGE,point,room,layer"|- SWAPS(comma) ["FRAME BUTTON HELD"...]: captures for the progress
# page, at 60 fps on the worker's GPU (quick; not for comparisons, which use llvmpipe): route save, the warp at
# f920 unless "-", then extra input lines after the save route's, in game frames as the routes' are (e.g.
# "1150 RLEFT 30" swings the camera); the captures' SWAPS are swaps, two a game frame from 900 (game frame 1150
# is swap 1400). WWHD_DEBUG_* and WWHD_60FPS_* pass through. Prints the captures (game data: they stay on the
# worker); then tools/progress/publish.sh shot PPM CAPTION. CEMU_BIN= another binary (one in a folder of its
# own runs beside a test). CAPTURE_30=1: at 30 instead (swaps are game frames then), to see whether something
# wrong at 60 is there at 30 too.
set -e
source "$(dirname "$0")/common.sh"
d=$OUT/cap-$1; rm -rf "$d"; mkdir -p "$d/shots"
st=$2; shots=$3; shift 3
grep -v '^#' tools/reference/routes/continue-100.txt > "$d/route.txt"
for l in "$@"; do echo "$l" >> "$d/route.txt"; done
last=$(echo "$shots" | tr , '\n' | sort -n | tail -1)
[ "$st" != - ] && export WWHD_DEBUG_STAGE="920:$st"
rate=(WWHD_60FPS=1 WWHD_60FPS_FROM=900); [ -n "${CAPTURE_30:-}" ] && rate=(WWHD_60FPS=)
env WWHD_EXIT_FRAME=$((last + 4)) "${rate[@]}" REF_SAVE=${REF_SAVE:-/wwhd/data/saves/wwhd_100} \
    CEMU_BIN="${CEMU_BIN:-$ROOT/build/wwhd/wwhd-null}" WWHD_NATIVE=on WWHD_VIRTUAL_SPEED=3 REF_FRESH=1 REF_VIRTUAL_CLOCK=1 \
    CEMU_INPUT_SCRIPT="$d/route.txt" CEMU_SHOT_FRAMES="$shots" CEMU_SHOT_DIR="$d/shots" WWHD_RENDER=vk \
    REF_PIDFILE="$d/pid" tools/reference/run.sh > "$d/run.log" 2>&1
while kill -0 "$(cat "$d/pid")" 2>/dev/null; do sleep 5; echo "capture: $(ls "$d/shots" | wc -l) files"; done
ls "$d"/shots/*.tv.ppm
