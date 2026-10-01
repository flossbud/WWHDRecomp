#!/usr/bin/env bash
# baseline.sh ROUTE  - record a new route's baselines (routes.sh names it), then check wwhd-null on it:
#   1. the reference twice (determinism.sh): the two OS-call traces must be equal; the first becomes
#      the route's trace baseline (routes.sh: trace);
#   2. Cemu's own libraries (build/wwhd-cemu: WWHD_FORKS=0 src/build.sh build/wwhd-cemu, run with
#      WWHD_OS=cemu) along the route: its trace must equal the reference's, and its GPU command stream
#      and sound become /wwhd/data/gx2/ROUTE-cemu.txt and ROUTE-audio-cemu.txt;
#   3. stream_check.sh ROUTE: wwhd-null's trace, command stream and sound against all three.
# Run it on the worker via tools/worker/job; it takes the reference's time twice plus two native runs.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
which=${1:?usage: baseline.sh ROUTE}
. "$here/routes.sh"
route_info "$which" || { echo "routes: $route_names" >&2; exit 2; }
[ -n "$save" ] && export REF_SAVE=$save
det=$(dirname "$trace")
echo "baseline $which: the reference twice ($frames frames)"
"$here/determinism.sh" "$det" "$frames" "$here/routes/$route"
[ "$det/a.zst" = "$trace" ] || cp "$det/a.zst" "$trace"
echo "baseline $which: Cemu's libraries"
out=/wwhd/data/gx2/$which-cemu-run
mkdir -p "$out"
CEMU_BIN=$root/build/wwhd-cemu/wwhd-null WWHD_OS=cemu WWHD_NATIVE=on WWHD_GPU_STREAM=$out/stream.txt WWHD_AUDIO_HASH=$out/audio.txt \
    "$here/route.sh" "$out/trace" "$frames" "$here/routes/$route" "$trace"
cp "$out/stream.txt" "/wwhd/data/gx2/$which-cemu.txt"
cp "$out/audio.txt" "/wwhd/data/gx2/$which-audio-cemu.txt"
echo "baseline $which: wwhd-null"
"$here/stream_check.sh" "$which" "ww-$which"
