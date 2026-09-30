#!/usr/bin/env bash
# stream_check.sh save|route NAME  - run wwhd-null natively along a scripted route (route.sh) and check
# both its OS-call trace and its GPU command stream (WWHD_GPU_STREAM: a hash per frame of every packet
# the GPU executes) against the baselines, recorded with Cemu's gx2. Our gx2 (src/os/gx2, design D18)
# must send exactly the commands Cemu's did. Exit 0 if both match; otherwise the first frame whose
# stream differs is printed.
#   save:  continue-100.txt from the 100% save, 1800 frames (~75 s)
#   route: title-to-game.txt, 10800 frames (~6 min, and ~3 min to compare the trace)
# Record new stream baselines with WWHD_OS=cemu into /wwhd/data/gx2/<save|route>-cemu.txt.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
which=${1:?usage: stream_check.sh save|route NAME}
name=${2:?name}
out=/wwhd/data/gx2/$name
mkdir -p "$out"
export CEMU_BIN=${CEMU_BIN:-$(cd "$here/../.." && pwd)/build/wwhd/wwhd-null} WWHD_NATIVE=${WWHD_NATIVE:-on}
export WWHD_GPU_STREAM=$out/stream.txt
case "$which" in
    save) REF_SAVE=/wwhd/data/saves/wwhd_100 "$here/route.sh" "$out/trace" 1800 "$here/routes/continue-100.txt" \
              /wwhd/data/traces/save-det/a.zst ;;
    route) "$here/route.sh" "$out/trace" 10800 "$here/routes/title-to-game.txt" /wwhd/data/traces/null-route.zst ;;
    *) echo "save or route" >&2; exit 2 ;;
esac
baseline=/wwhd/data/gx2/$which-cemu.txt
if cmp -s "$WWHD_GPU_STREAM" "$baseline"; then
    echo "STREAM MATCHES $baseline ($(wc -l < "$baseline") frames, $(awk '{p += $3} END {print p}' "$baseline") packets)"
else
    echo "STREAM DIFFERS from $baseline, first at:"
    diff <(cat "$WWHD_GPU_STREAM") "$baseline" | head -4
    exit 1
fi
