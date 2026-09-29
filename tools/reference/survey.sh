#!/usr/bin/env bash
# survey.sh ROUTE OUT_DIR LAST_FRAME [STEP=30] [FIRST=0]  - fresh boot with the virtual clock, playing ROUTE
# (a CEMU_INPUT_SCRIPT file), capturing the TV every STEP frames up to LAST_FRAME into OUT_DIR, then
# a labelled contact sheet OUT_DIR/contact.png (captures start at FIRST). Used to find the frames at which to put the next
# route inputs. Runs 30 frames past LAST so the (render-thread) capture of LAST lands. Prints the newest captured frame once a minute as a progress heartbeat
# (tools/worker/job wait treats a silent log as stalled). Run on the worker via tools/worker/job.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
route=$(readlink -f "${1:?usage: survey.sh ROUTE OUT_DIR LAST_FRAME [STEP]}")
out=${2:?}; last=${3:?}; step=${4:-30}; first=${5:-0}
mkdir -p "$out"; rm -f "$out"/f*.ppm "$out"/f*.part "$out"/contact.png
export REF_PIDFILE=$out/cemu.pid
REF_FRESH=1 REF_VIRTUAL_CLOCK=1 CEMU_INPUT_SCRIPT=$route CEMU_SHOT_FRAMES=$first-$last/$step CEMU_SHOT_DIR=$out \
    CEMU_HLE_TRACE=$out/trace.zst CEMU_HLE_TRACE_FILTER=gx2.GX2SwapScanBuffers CEMU_HLE_TRACE_EXIT_FRAME=$((last + 30)) \
    "$here/run.sh"
while kill -0 "$(cat "$REF_PIDFILE")" 2>/dev/null; do
    sleep 60
    echo "survey: newest capture $(ls "$out" | grep -o 'f[0-9]*' | sort | tail -1)"
done
montage -label %f "$out"/f*.tv.ppm -tile 6x -geometry 240x135+3+3 -pointsize 11 "$out/contact.png"
echo "survey: $(ls "$out"/f*.tv.ppm | wc -l) captures, contact sheet $out/contact.png"
