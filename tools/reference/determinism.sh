#!/usr/bin/env bash
# determinism.sh OUT_DIR [FRAMES] [ROUTE]  - run the patched reference twice from a fresh NAND with the
# virtual clock, tracing every OS call until frame FRAMES (default 600), then compare the traces.
# ROUTE is an input script (routes/*.txt) both runs play; each run also captures the TV every
# FRAMES/8 frames into OUT_DIR/{a,b}/ so you can see where the route got to.
# Exit 0 if the two runs made identical call streams.
#   REF_MAX_MINUTES  per-run wall-clock limit (default 30); a run still going then is killed
#                    and the check fails, rather than silently running for hours
#   CEMU_BIN  patched build (default: the worker's /wwhd/opt/cemu-src/bin/Cemu_release); WWHD_GAME as for run.sh
# Run it on the worker: tools/worker/w tools/reference/determinism.sh /wwhd/data/traces/det1
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: determinism.sh OUT_DIR [FRAMES] [ROUTE]}
frames=${2:-600}
route=${3:+$(readlink -f "$3")}
mkdir -p "$out"
export CEMU_BIN=${CEMU_BIN:-/wwhd/opt/cemu-src/bin/Cemu_release}
for run in a b; do
    mkdir -p "$out/$run"; rm -f "$out/$run"/f*.ppm
    REF_FRESH=1 REF_VIRTUAL_CLOCK=1 CEMU_HLE_TRACE="$out/$run.zst" CEMU_HLE_TRACE_EXIT_FRAME=$frames \
        CEMU_INPUT_SCRIPT=${route:-} CEMU_SHOT_FRAMES=0-$frames/$(( frames / 8 )) CEMU_SHOT_DIR="$out/$run" \
        "$here/run.sh"
    deadline=$(( $(date +%s) + ${REF_MAX_MINUTES:-30} * 60 ))
    while pgrep -x cemu >/dev/null; do                   # Cemu exits itself at the frame limit
        if [ "$(date +%s)" -gt "$deadline" ]; then
            pkill -x cemu || true
            echo "run $run: did not reach frame $frames within ${REF_MAX_MINUTES:-30} min; stopped" >&2
            exit 3
        fi
        sleep 5
        [ $(( $(date +%s) % 60 )) -lt 5 ] && echo "run $run: trace $(stat -c %s "$out/$run.zst" 2>/dev/null || echo 0) bytes"   # heartbeat
    done
    echo "run $run: $(stat -c %s "$out/$run.zst") bytes"
done
# Byte-identical is the fast path; otherwise compare normalized records (Cemu names some callback
# stubs after host pointers, which ASLR changes every run).
if cmp -s <(zstd -dcq "$out/a.zst") <(zstd -dcq "$out/b.zst") \
   || uv run -q "$here/hle_trace.py" diff "$out/a.zst" "$out/b.zst"; then
    echo "DETERMINISTIC: identical traces for $frames frames${route:+ along $route}"
else
    exit 1
fi
