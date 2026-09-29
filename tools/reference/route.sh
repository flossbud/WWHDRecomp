#!/usr/bin/env bash
# route.sh OUT_DIR FRAMES ROUTE [BASELINE.zst]  - one run of CEMU_BIN along ROUTE with the virtual
# clock, tracing every OS call until frame FRAMES into OUT_DIR/a.zst, then (with BASELINE) compare
# the trace with a baseline trace. determinism.sh runs twice to show a build is deterministic; this
# checks a change against a known-good trace in half the time (e.g. wwhd-null in diff mode against
# /wwhd/data/traces/null-route.zst). Exit 0 if the traces match (or no baseline was given).
#   CEMU_BIN  as for determinism.sh; REF_MAX_MINUTES  wall-clock limit (default 30)
# Run it on the worker: tools/worker/job start NAME tools/reference/route.sh OUT 10800 ROUTE BASELINE
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: route.sh OUT_DIR FRAMES ROUTE [BASELINE.zst]}
frames=${2:?frames}
route=$(readlink -f "${3:?route}")
baseline=${4:-}
mkdir -p "$out/a"; rm -f "$out/a"/f*.ppm
export CEMU_BIN=${CEMU_BIN:-/wwhd/opt/cemu-src/bin/Cemu_release}
export REF_PIDFILE=$out/cemu.pid
start=$(date +%s)
REF_FRESH=1 REF_VIRTUAL_CLOCK=1 CEMU_HLE_TRACE="$out/a.zst" CEMU_HLE_TRACE_EXIT_FRAME=$frames \
    CEMU_INPUT_SCRIPT=$route CEMU_SHOT_FRAMES=0-$frames/$(( frames / 8 )) CEMU_SHOT_DIR="$out/a" \
    "$here/run.sh"
deadline=$(( start + ${REF_MAX_MINUTES:-30} * 60 ))
while kill -0 "$(cat "$REF_PIDFILE")" 2>/dev/null; do                  # Cemu exits itself at the frame limit
    if [ "$(date +%s)" -gt "$deadline" ]; then
        kill "$(cat "$REF_PIDFILE")" || true
        echo "did not reach frame $frames within ${REF_MAX_MINUTES:-30} min; stopped" >&2
        exit 3
    fi
    sleep 5
    [ $(( $(date +%s) % 60 )) -lt 5 ] && echo "trace $(stat -c %s "$out/a.zst" 2>/dev/null || echo 0) bytes"   # heartbeat
done
echo "run: $(stat -c %s "$out/a.zst") bytes in $(( $(date +%s) - start ))s"
[ -n "$baseline" ] || exit 0
if uv run -q "$here/hle_trace.py" diff "$out/a.zst" "$baseline"; then   # prints the call count
    echo "MATCHES $baseline (frames 0-$frames along $route)"
else
    exit 1
fi
