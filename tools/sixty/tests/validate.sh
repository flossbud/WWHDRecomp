#!/usr/bin/env bash
# validate.sh [ROUTE...]: Vulkan validation of the renderer over routes (b-1dviews, session bottom). Each route is
# rendered (WWHD_RENDER=vk, on the machine's GPU, at 30) under the Khronos validation layer, its messages written to a file
# of the route's own (VK_KHRONOS_VALIDATION_LOG_FILENAME): run.sh drops the game's output, so a layer reporting there
# reported to nobody, and an error went unnoticed for a round. Each route's binary, log and messages stay in
# $OUT/validate/ROUTE (the game's log as log.txt beside them).
#   default routes: every route routes.sh knows (route_names), each to its end
#   VALIDATE_FRAMES=N    end every route at game frame N (at most its own end)
#   VALIDATE_JOBS=N      routes at once (default 4; each a binary of its own)
#   VALIDATE_ENV="K=V K=V"  options for every run (e.g. "WWHD_RENDER_SCALE=0.5 WWHD_UPSCALER=fsr1 WWHD_FXAA=1")
#   VALIDATE_SYNC=1      synchronization validation too (slower)
#   VALIDATE_BIN=path    the binary (default build/wwhd/wwhd-null)
#   VALIDATE_LLVMPIPE=1  render in software instead (a driver of its own, but ~10x slower: 15 min a route)
# Out: per route, its message IDs and counts; then the totals. Exit status 1 when any route has an error, 2 when a run
# left no game log (a crash before the log, or a bad route) or no file from the layer (it didn't load). Run it as a job: tools/worker/job start val
# tools/sixty/tests/validate.sh
set -uo pipefail
cd "$(dirname "$0")/../../.."
source tools/sixty/tests/common.sh
. tools/reference/routes.sh
bin=${VALIDATE_BIN:-build/wwhd/wwhd-null}
[ -x "$bin" ] || { echo "validate: no $bin"; exit 2; }
# the layer must be there: without it every run is "clean" (the worker's container had none: a false pass)
layer=$(ls ${VK_LAYER_PATH:+$VK_LAYER_PATH/}*khronos_validation*.json /usr/share/vulkan/explicit_layer.d/*khronos_validation*.json \
    /etc/vulkan/explicit_layer.d/*khronos_validation*.json "$HOME"/.local/share/vulkan/explicit_layer.d/*khronos_validation*.json 2>/dev/null | head -1)
[ -n "$layer" ] || { echo "validate: no Khronos validation layer on this machine (package vulkan-validationlayers)"; exit 2; }
routes=("$@"); [ ${#routes[@]} -eq 0 ] && read -ra routes <<< "$route_names"
jobs=${VALIDATE_JOBS:-4}
base=$OUT/validate; mkdir -p "$base"

one() {
    local r=$1 d=$base/$1
    route_info "$r" >/dev/null 2>&1 || { echo "$r: not a route"; return; }
    local n=$frames; [ -n "${VALIDATE_FRAMES:-}" ] && [ "$VALIDATE_FRAMES" -lt "$n" ] && n=$VALIDATE_FRAMES
    rm -rf "$d"; mkdir -p "$d"; cp "$bin" "$d/"
    env ${VALIDATE_ENV:-} VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_KHRONOS_VALIDATION_LOG_FILENAME="$d/val.txt" \
        VK_KHRONOS_VALIDATION_REPORT_FLAGS=error ${VALIDATE_SYNC:+VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true} \
        CEMU_BIN="$d/$(basename "$bin")" WWHD_RENDER=vk ${VALIDATE_LLVMPIPE:+REF_GPU=llvmpipe} SIXTY_FRAMES=$n \
        tools/sixty/run.sh "$r" "$d/run" 30 > "$d/run.txt" 2>&1
    [ -f "$d/portable/log.txt" ] && cp "$d/portable/log.txt" "$d/log.txt"
    rm -rf "$d/run" "$d/portable/mlc01" "$d/portable/shaderCache" "$d/$(basename "$bin")"
    if [ ! -f "$d/log.txt" ]; then echo "$r: NO LOG (to frame $n; $d/run.txt)"; return; fi
    # the layer writes its file at start even with nothing to report: none means it never loaded
    if [ ! -f "$d/val.txt" ]; then echo "$r: NO LOG from the layer (to frame $n: not validated)"; return; fi
    local ids; ids=$( { grep -o "VUID-[A-Za-z0-9_-]*\|SYNC-HAZARD-[A-Z_-]*\|UNASSIGNED-[A-Za-z0-9_.-]*" "$d/val.txt" 2>/dev/null || true; } |
        sed 's/-$//' | sort | uniq -c | sort -rn | awk '{printf " %s x%s", $2, $1}')
    echo "$r: to frame $n:${ids:- clean}"
}

echo "validate: ${#routes[@]} routes, $jobs at once${VALIDATE_ENV:+, with $VALIDATE_ENV}${VALIDATE_SYNC:+, synchronization too} -> $base"
res=$base/results.txt; : > "$res"
running=0
for r in "${routes[@]}"; do
    one "$r" >> "$res" &
    running=$((running + 1))
    if [ $running -ge $jobs ]; then wait -n; running=$((running - 1)); fi
done
wait
sort "$res"
bad=$(grep -c "VUID-\|SYNC-HAZARD\|UNASSIGNED-" "$res")
nolog=$(grep -c "NO LOG" "$res")   # a run without its log or without the layer's
echo "validate: ${#routes[@]} routes, $bad with errors, $nolog without a log"
echo "validate: totals:$(grep -o "\(VUID\|SYNC-HAZARD\|UNASSIGNED\)-[A-Za-z0-9_.-]* x[0-9]*" "$res" | awk '{split($2, c, "x"); t[$1] += c[2]} END {for (k in t) printf " %s x%d", k, t[k]}')"
[ "$nolog" -gt 0 ] && exit 2
[ "$bad" -gt 0 ] && exit 1
exit 0
