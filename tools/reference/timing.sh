#!/usr/bin/env bash
# timing.sh save|route OUT_DIR  - how fast wwhd-null runs a scripted route natively, without the trace
# (which costs ~40%): the virtual clock keeps the work identical from run to run, WWHD_EXIT_FRAME
# ends it, and the profiler (WWHD_PROFILE, src/runtime/profile.cpp) records where the CPU time went.
# Prints wall time, speed against the game's 30 frames a second and CPU time per thread; the full
# report: python3 tools/profile_report.py OUT_DIR/profile.txt. Extra environment passes through
# (e.g. WWHD_RENDER=vk REF_GPU=llvmpipe). Run it on the worker: tools/worker/job start NAME ...
#   save:  continue-100.txt from the 100% save, 1800 frames
#   route: title-to-game.txt, 10800 frames
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
which=${1:?usage: timing.sh save|route OUT_DIR}
out=${2:?out dir}
mkdir -p "$out"
case "$which" in
    save) frames=1800 route=$here/routes/continue-100.txt; export REF_SAVE=/wwhd/data/saves/wwhd_100 ;;
    route) frames=10800 route=$here/routes/title-to-game.txt ;;
    *) echo "save or route" >&2; exit 2 ;;
esac
export CEMU_BIN=${CEMU_BIN:-$(cd "$here/../.." && pwd)/build/wwhd/wwhd-null} WWHD_NATIVE=${WWHD_NATIVE:-on}
export REF_PIDFILE=$out/pid WWHD_PROFILE=$out/profile.txt WWHD_EXIT_FRAME=$frames
start=$(date +%s.%N)
REF_FRESH=1 REF_VIRTUAL_CLOCK=1 CEMU_INPUT_SCRIPT=$route "$here/run.sh"
while kill -0 "$(cat "$REF_PIDFILE")" 2>/dev/null; do sleep 0.2; done
end=$(date +%s.%N)
python3 - "$out/profile.txt" "$start" "$end" "$frames" <<'PY'
import sys
path, start, end, frames = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), int(sys.argv[4])
wall = end - start
tick, threads = 100, {}
for line in open(path):
    p = line.split()
    if p and p[0] == 'ticks_per_second':
        tick = int(p[1])
    elif p and p[0] == 'thread':
        name = ' '.join(p[4:])
        threads[name] = threads.get(name, 0) + int(p[2]) + int(p[3])
print(f"{frames} frames in {wall:.1f} s (from launch): {frames / 30 / wall:.2f}x real time")
for name, t in sorted(threads.items(), key=lambda kv: -kv[1]):
    if t:
        print(f"  {t / tick:7.1f} s CPU  {name}")
PY
