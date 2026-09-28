#!/usr/bin/env bash
# determinism.sh OUT_DIR [FRAMES]  - run the patched reference twice from a fresh NAND with the
# virtual clock, tracing every OS call until frame FRAMES (default 600), then compare the traces.
# Exit 0 if the two runs made byte-identical call streams.
#   CEMU_BIN  patched build (default ~/opt/cemu-src/bin/Cemu_release); WWHD_GAME as for run.sh
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: determinism.sh OUT_DIR [FRAMES]}
frames=${2:-600}
mkdir -p "$out"
export CEMU_BIN=${CEMU_BIN:-$HOME/opt/cemu-src/bin/Cemu_release}
for run in a b; do
    REF_FRESH=1 REF_VIRTUAL_CLOCK=1 CEMU_HLE_TRACE="$out/$run.zst" CEMU_HLE_TRACE_EXIT_FRAME=$frames \
        "$here/run.sh"
    while pgrep -x cemu >/dev/null; do sleep 5; done   # Cemu exits itself at the frame limit
    echo "run $run: $(stat -c %s "$out/$run.zst") bytes"
done
if cmp <(zstd -dcq "$out/a.zst") <(zstd -dcq "$out/b.zst"); then
    echo "DETERMINISTIC: identical traces for $frames frames"
else
    uv run -q "$here/hle_trace.py" diff "$out/a.zst" "$out/b.zst" || true
    exit 1
fi
