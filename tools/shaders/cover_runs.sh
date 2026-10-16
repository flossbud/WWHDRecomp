#!/usr/bin/env bash
# cover_runs.sh OUT JOBS ITEM...  - capturing runs for the shader list (s-shadercover): each ITEM played with the virtual
# clock at 30, rendered (WWHD_RENDER=vk: the machine's GPU; REF_GPU=llvmpipe for software), from an empty shader cache
# with WWHD_SHADER_LIST=none and WWHD_SHADER_SOURCES=OUT/ITEM.sources (everything met for the first time:
# tools/shaders/shader_list.py merges them into config/US_v0/shader_list.txt). JOBS runs at once, each slot with its own
# binary copy (its portable folder: one game per folder). An ITEM is a route name (tools/reference/routes.sh, played by
# tools/sixty/run.sh) or a survey chunk's path without its extension (tools/shaders/survey_route.py: CHUNK.env holds its
# WWHD_DEBUG_STAGE warps and length, CHUNK.txt its input), which also captures the TV once a visit, 240 frames after
# each warp, into OUT/ITEM.shots (game pictures: they stay on the worker). Each item's emulator log: OUT/ITEM.log.
# LIST=path: the check instead (no capture): each item from an empty cache with that list, and OUT/ITEM.check says how
# many shaders and pipelines the list prepared and how many records the cache held at the end: more is a first sight
# during play (the shader list's gate: none on the covered routes).
# BIN= another wwhd-null (default build/wwhd/wwhd-null). An item that finished (OUT/ITEM.done) is skipped, so a stopped
# run started again plays only what it hadn't finished. Run on a worker via tools/worker/job.
set -uo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${1:?usage: cover_runs.sh OUT JOBS ITEM...}; jobs=${2:?jobs}; shift 2
mkdir -p "$out"
bin=${BIN:-$root/build/wwhd/wwhd-null}
run_one() {
    local item=$1 slot name; name=$(basename "$item")
    for slot in $(seq 0 $((jobs - 1))); do mkdir "$out/slot$slot.lock" 2>/dev/null && break; done   # a free slot: at most JOBS run
    local sb=$out/slot$slot; mkdir -p "$sb"; cp "$bin" "$sb/wwhd-null"
    rm -rf "$sb/portable/shaderCache"; rm -f "$out/$name.sources"
    local common=(WWHD_RENDER=vk WWHD_SHADER_LIST=none WWHD_SHADER_SOURCES=$out/$name.sources CEMU_BIN=$sb/wwhd-null)
    [ -n "${LIST:-}" ] && common=(WWHD_RENDER=vk WWHD_SHADER_LIST=$LIST CEMU_BIN=$sb/wwhd-null)
    local start; start=$(date +%s)
    if [ -f "$item.env" ]; then
        local stage frames
        stage=$(sed -n 's/^WWHD_DEBUG_STAGE=//p' "$item.env"); frames=$(sed -n 's/^SIXTY_FRAMES=//p' "$item.env")
        rm -rf "$out/$name.shots"; mkdir -p "$out/$name.shots"
        env "${common[@]}" WWHD_DEBUG_STAGE="$stage" WWHD_NATIVE=on REF_FRESH=1 REF_VIRTUAL_CLOCK=1 WWHD_VIRTUAL_SPEED=3 \
            REF_SAVE=/wwhd/data/saves/wwhd_100 CEMU_INPUT_SCRIPT="$item.txt" CEMU_SHOT_FRAMES=1160-$frames/270 \
            CEMU_SHOT_DIR="$out/$name.shots" WWHD_EXIT_FRAME=$frames REF_PIDFILE="$sb/pid" \
            "$root/tools/reference/run.sh" > "$out/$name.out" 2>&1
        while kill -0 "$(cat "$sb/pid")" 2>/dev/null; do sleep 5; done
    else
        env "${common[@]}" "$root/tools/sixty/run.sh" "$item" "$sb/run" 30 > "$out/$name.out" 2>&1
    fi
    cp "$sb/portable/log.txt" "$out/$name.log" 2>/dev/null
    if [ -n "${LIST:-}" ]; then
        python3 - "$sb/portable/shaderCache/wwhd" "$out/$name.log" > "$out/$name.check" <<'PY'
import re, sys
def records(path):
    try:
        data = open(path, "rb").read()
    except OSError:
        return 0
    pos, n = 16, 0
    while pos + 4 <= len(data):
        n += 1
        pos += 4 + int.from_bytes(data[pos:pos + 4], "little") + 8
    return n
m = re.search(r"prepared (\d+) shaders .* and (\d+) pipelines", open(sys.argv[2], errors="replace").read())
ps, pp = (int(m.group(1)), int(m.group(2))) if m else (-1, -1)
s, p = records(sys.argv[1] + "/shaders.bin"), records(sys.argv[1] + "/pipelines.bin")
print(f"prepared {ps} shaders {pp} pipelines; at the end {s} and {p}: first sights {s - ps} shaders, {p - pp} pipelines")
PY
        echo "cover_runs: $name in $(( $(date +%s) - start )) s: $(cat "$out/$name.check")"
    else
        echo "cover_runs: $name in $(( $(date +%s) - start )) s: $(wc -l < "$out/$name.sources" 2>/dev/null || echo 0) source lines"
    fi
    touch "$out/$name.done"
    rmdir "$out/slot$slot.lock"
}
rmdir "$out"/slot*.lock 2>/dev/null
i=0
for item in "$@"; do
    [ -e "$out/$(basename "$item").done" ] && continue
    while [ "$(jobs -rp | wc -l)" -ge "$jobs" ]; do wait -n; done
    run_one "$item" &
    i=$((i + 1))
    sleep 2
done
wait
echo "cover_runs: $i items done"
