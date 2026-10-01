#!/usr/bin/env bash
# shader_list_check.sh OUT_DIR [LAST_FRAME=900]  - the shader list (design D20: a first start without
# hitches) prepares everything before play, and changes nothing on screen. Two runs of the save route
# with the virtual clock, rendering on lavapipe, each from an empty shader cache:
#   capture: no list; WWHD_SHADER_SOURCES records everything met for the first time, and checks each
#            shader translates the same from the registers its line keeps;
#   first:   the list made from the capture (tools/shaders/shader_list.py) and nothing else.
# Their TV captures (every 30 frames) must be the same bytes, and the first run's cache must hold the
# same shader and pipeline records as the capture run's: all of them prepared before the game started
# (translated from the game's files), none met during play. Run on the worker via tools/worker/job.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=${1:?usage: shader_list_check.sh OUT_DIR [LAST_FRAME]}; last=${2:-900}
bin=$root/build/wwhd/wwhd-null
portable=$(dirname "$bin")/portable
cache=$portable/shaderCache/wwhd
rm -rf "$out"
for run in capture first; do
    mkdir -p "$out/$run"
    rm -rf "$cache"
    if [ $run = capture ]; then extra=(WWHD_SHADER_LIST=none WWHD_SHADER_SOURCES=$out/sources.txt)
    else extra=(WWHD_SHADER_LIST=$out/list.txt); fi
    env "${extra[@]}" CEMU_BIN=$bin WWHD_NATIVE=on WWHD_RENDER=vk REF_GPU=llvmpipe REF_FRESH=1 REF_VIRTUAL_CLOCK=1 \
        REF_SAVE=/wwhd/data/saves/wwhd_100 CEMU_INPUT_SCRIPT=$here/routes/continue-100.txt \
        CEMU_SHOT_FRAMES=30-$last/30 CEMU_SHOT_DIR=$out/$run WWHD_EXIT_FRAME=$((last + 30)) REF_PIDFILE=$out/$run/pid \
        "$here/run.sh"
    while kill -0 "$(cat "$out/$run/pid")" 2>/dev/null; do sleep 5; echo "... $run running"; done
    echo "$run: $(grep -h 'shader cache:\|shader list:' "$portable/log.txt" | sed 's/^\[[^]]*\] //' | tr '\n' ' ')"
    cp "$cache"/shaders.bin "$cache"/pipelines.bin "$out/$run/"
    [ $run = capture ] && python3 "$root/tools/shaders/shader_list.py" "$out/list.txt" "$out/sources.txt"
done
n=0 bad=0
for f in "$out"/capture/f*.tv.ppm; do
    n=$((n + 1))
    cmp -s "$f" "$out/first/$(basename "$f")" || { bad=$((bad + 1)); echo "differs: $(basename "$f")"; }
done
echo "$n captures, $bad differ"
# the same records (any order): every shader translated from the list exactly as play translated it
python3 - "$out" <<'PY'
import sys
out = sys.argv[1]
def records(path):
    data = open(path, "rb").read()
    pos, recs = 16, []
    while pos + 4 <= len(data):
        n = int.from_bytes(data[pos:pos + 4], "little")
        recs.append(data[pos + 4:pos + 4 + n])
        pos += 4 + n + 8
    return recs
ok = True
for name in ("shaders.bin", "pipelines.bin"):
    a, b = records(f"{out}/capture/{name}"), records(f"{out}/first/{name}")
    same = sorted(a) == sorted(b)
    ok &= same
    print(f"{name}: capture {len(a)} records, first start {len(b)}: {'the same' if same else 'DIFFERENT'}")
sys.exit(0 if ok else 1)
PY
[ "$n" -gt 0 ] && [ "$bad" = 0 ]
