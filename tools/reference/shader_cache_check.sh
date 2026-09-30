#!/usr/bin/env bash
# shader_cache_check.sh OUT_DIR [LAST_FRAME=900]  - the shader cache (design D20) changes nothing on
# screen. Two runs of the save route with the virtual clock, rendering on lavapipe: the first starts
# with an empty cache and fills it, the second prepares everything from it before the game starts.
# Their TV captures (every 30 frames) must be the same bytes, and the second must add nothing to the
# cache (no shader translated, no pipeline built during play). Run on the worker via tools/worker/job.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: shader_cache_check.sh OUT_DIR [LAST_FRAME]}; last=${2:-900}
bin=$(cd "$here/../.." && pwd)/build/wwhd/wwhd-null
portable=$(dirname "$bin")/portable
cache=$portable/shaderCache/wwhd
rm -rf "$out" "$cache"
for run in cold warm; do
    mkdir -p "$out/$run"
    CEMU_BIN=$bin WWHD_NATIVE=on WWHD_RENDER=vk REF_GPU=llvmpipe REF_FRESH=1 REF_VIRTUAL_CLOCK=1 \
        REF_SAVE=/wwhd/data/saves/wwhd_100 CEMU_INPUT_SCRIPT=$here/routes/continue-100.txt \
        CEMU_SHOT_FRAMES=30-$last/30 CEMU_SHOT_DIR=$out/$run WWHD_EXIT_FRAME=$((last + 30)) REF_PIDFILE=$out/$run/pid \
        "$here/run.sh"
    while kill -0 "$(cat "$out/$run/pid")" 2>/dev/null; do sleep 5; done
    echo "$run: $(grep -h 'shader cache:' "$portable/log.txt" | sed 's/^\[[^]]*\] //' | tr '\n' ' ')"
    stat -c '%n %s' "$cache"/*.bin | sed "s|$cache/|  |" | tee "$out/$run/cache.txt"
done
n=0 bad=0
for f in "$out"/cold/f*.tv.ppm; do
    n=$((n + 1))
    cmp -s "$f" "$out/warm/$(basename "$f")" || { bad=$((bad + 1)); echo "differs: $(basename "$f")"; }
done
grs() { grep -E 'shaders.bin|pipelines.bin' "$1"; }
added=$(diff <(grs "$out/cold/cache.txt") <(grs "$out/warm/cache.txt") >/dev/null && echo nothing || echo something)
echo "$n captures, $bad differ; the warm run added $added to the cache"
[ "$n" -gt 0 ] && [ "$bad" = 0 ] && [ "$added" = nothing ]
