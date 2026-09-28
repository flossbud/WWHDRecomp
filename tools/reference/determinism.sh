#!/usr/bin/env bash
# determinism.sh OUT_DIR [FRAMES]  - run the patched reference twice from a fresh NAND with the
# virtual clock, tracing every OS call until frame FRAMES (default 600), then compare the traces.
# Exit 0 if the two runs made byte-identical call streams.
#   REF_MAX_MINUTES  per-run wall-clock limit (default 30); a run still going then is killed
#                    and the check fails, rather than silently running for hours
#   CEMU_BIN  patched build (default: the worker's /wwhd/opt/cemu-src/bin/Cemu_release); WWHD_GAME as for run.sh
# Run it on the worker: tools/worker/w tools/reference/determinism.sh /wwhd/data/traces/det1
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: determinism.sh OUT_DIR [FRAMES]}
frames=${2:-600}
mkdir -p "$out"
export CEMU_BIN=${CEMU_BIN:-/wwhd/opt/cemu-src/bin/Cemu_release}
for run in a b; do
    REF_FRESH=1 REF_VIRTUAL_CLOCK=1 CEMU_HLE_TRACE="$out/$run.zst" CEMU_HLE_TRACE_EXIT_FRAME=$frames \
        "$here/run.sh"
    deadline=$(( $(date +%s) + ${REF_MAX_MINUTES:-30} * 60 ))
    while pgrep -x cemu >/dev/null; do                   # Cemu exits itself at the frame limit
        if [ "$(date +%s)" -gt "$deadline" ]; then
            pkill -x cemu || true
            echo "run $run: did not reach frame $frames within ${REF_MAX_MINUTES:-30} min; stopped" >&2
            exit 3
        fi
        sleep 5
    done
    echo "run $run: $(stat -c %s "$out/$run.zst") bytes"
done
if cmp <(zstd -dcq "$out/a.zst") <(zstd -dcq "$out/b.zst"); then
    echo "DETERMINISTIC: identical traces for $frames frames"
else
    uv run -q "$here/hle_trace.py" diff "$out/a.zst" "$out/b.zst" || true
    exit 1
fi
