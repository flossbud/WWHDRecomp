#!/usr/bin/env bash
# run.sh ROUTE OUT_DIR [30|60]... - play a scripted route with the virtual clock at 30 and/or 60 ticks
# a second (docs/recompiler-design.md D21) with the state probe on (WWHD_STATE_DUMP), for
# tools/sixty/compare.py. ROUTE is a name from tools/reference/routes.sh. At 60 the route plays at 30
# up to swap SIXTY_FROM (default 900: the routes from the 100% save are on the dock by then), then the
# game presents every vsync (WWHD_60FPS=1, WWHD_60FPS_FROM) and each game frame takes two swaps, so
# the run ends at FROM + 2 (frames - FROM). SIXTY_TRACE=FILTER also writes each run's OS-call trace
# of the functions FILTER names (CEMU_HLE_TRACE_FILTER prefixes) to OUT/RATE/trace.zst, for
# tools/sixty/sound.py. Both rates run with WWHD_VIRTUAL_SPEED (default 3 here):
# the emulated CPU that fast, so that a 60 fps frame fits in one vsync of guest time and both runs
# keep the same guest time per game frame. SIXTY_FRAMES=N ends the route early, at game frame N.
# Extra environment passes through (WWHD_STATE_DUMP_EVERY, CEMU_SHOT_FRAMES with doubled
# frames at 60, WWHD_RENDER=vk REF_GPU=llvmpipe). Run it on the worker: tools/worker/job start NAME ...
# The dumps are game memory: keep OUT_DIR on the worker.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
ref=$here/../reference
which=${1:?usage: run.sh ROUTE OUT_DIR [30|60]...}
out=${2:?out dir}
shift 2
rates=("$@"); [ ${#rates[@]} -gt 0 ] || rates=(30 60)
. "$ref/routes.sh"
route_info "$which" || { echo "routes: $route_names" >&2; exit 2; }
frames=${SIXTY_FRAMES:-$frames}                    # SIXTY_FRAMES=N ends the route at game frame N
# a route's own setup: its "#env NAME=VALUE" lines (stage warps, pokes, spawns), exported unless already set
while IFS= read -r line; do
    kv=${line#\#env }; name=${kv%%=*}
    if [ -z "${!name:-}" ]; then export "$name=${kv#*=}"; fi
done < <(grep '^#env [A-Z_0-9]*=' "$ref/routes/$route" || true)
[ -n "$save" ] && export REF_SAVE=$save
export CEMU_BIN=${CEMU_BIN:-$(cd "$here/../.." && pwd)/build/wwhd/wwhd-null} WWHD_NATIVE=${WWHD_NATIVE:-on}
# SIXTY_REALTIME=1: real time instead of the virtual clock (no virtual speed): for comparing real-time modes
# (tools/sixty/tests/rt_routes.sh); the checks never use it
[ -z "${SIXTY_REALTIME:-}" ] && export WWHD_VIRTUAL_SPEED=${WWHD_VIRTUAL_SPEED:-3}
for rate in "${rates[@]}"; do
    dir=$out/$rate
    rm -rf "$dir" && mkdir -p "$dir"
    from=${SIXTY_FROM:-900}
    if [ "$rate" = 60 ]; then sixty=1 exit_frame=$((from + 2 * (frames - from))); else sixty= exit_frame=$frames; fi
    echo "run.sh: $which at $rate ticks a second, to swap $exit_frame, state in $dir"
    start=$(date +%s)
    # the trace ends the run itself (and closes its file); otherwise WWHD_EXIT_FRAME does
    ending=(WWHD_EXIT_FRAME=$exit_frame)
    [ -n "${SIXTY_TRACE:-}" ] && ending=(CEMU_HLE_TRACE=$dir/trace.zst CEMU_HLE_TRACE_FILTER=$SIXTY_TRACE CEMU_HLE_TRACE_EXIT_FRAME=$exit_frame)
    env "${ending[@]}" WWHD_60FPS=$sixty WWHD_60FPS_FROM=$from WWHD_STATE_DUMP=$dir REF_PIDFILE=$dir/pid \
        REF_FRESH=1 REF_VIRTUAL_CLOCK=$([ -z "${SIXTY_REALTIME:-}" ] && echo 1) CEMU_INPUT_SCRIPT=$ref/routes/$route "$ref/run.sh"
    while kill -0 "$(cat "$dir/pid")" 2>/dev/null; do sleep 5; echo "run.sh: $rate: $(wc -l < "$dir/hashes.txt" 2>/dev/null || echo 0) actor states"; done
    echo "run.sh: $rate done in $(( $(date +%s) - start )) s: $(tail -1 "$dir/hashes.txt" | cut -d' ' -f1) whole ticks"
done
