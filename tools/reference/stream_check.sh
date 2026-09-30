#!/usr/bin/env bash
# stream_check.sh save|route NAME  - run wwhd-null natively along a scripted route (route.sh) and check
# its OS-call trace, its GPU command stream (WWHD_GPU_STREAM: a hash per frame of every packet the GPU
# executes) and its sound (WWHD_AUDIO_HASH: a hash of every mixed block) against baselines recorded
# with Cemu's gx2 and snd_core. Ours (src/os, design D18) must send exactly the commands and mix
# exactly the sound Cemu's did. Exit 0 if all match; otherwise the first difference is printed.
#   save:  continue-100.txt from the 100% save, 1800 frames (~75 s)
#   route: title-to-game.txt, 10800 frames (~6 min, and ~3 min to compare the trace)
# Baselines: /wwhd/data/gx2/<save|route>-cemu.txt (commands), <save|route>-audio-cemu.txt (sound),
# recorded with Cemu's gx2 and snd_core: WWHD_FORKS=0 src/build.sh build/wwhd-cemu, then
# CEMU_BIN=build/wwhd-cemu/wwhd-null WWHD_OS=cemu stream_check.sh ... and copy stream.txt/audio.txt.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
which=${1:?usage: stream_check.sh save|route NAME}
name=${2:?name}
out=/wwhd/data/gx2/$name
mkdir -p "$out"
export CEMU_BIN=${CEMU_BIN:-$(cd "$here/../.." && pwd)/build/wwhd/wwhd-null} WWHD_NATIVE=${WWHD_NATIVE:-on}
export WWHD_GPU_STREAM=$out/stream.txt WWHD_AUDIO_HASH=$out/audio.txt
case "$which" in
    save) REF_SAVE=/wwhd/data/saves/wwhd_100 "$here/route.sh" "$out/trace" 1800 "$here/routes/continue-100.txt" \
              /wwhd/data/traces/save-det/a.zst ;;
    route) "$here/route.sh" "$out/trace" 10800 "$here/routes/title-to-game.txt" /wwhd/data/traces/null-route.zst ;;
    *) echo "save or route" >&2; exit 2 ;;
esac
status=0
baseline=/wwhd/data/gx2/$which-cemu.txt
if cmp -s "$WWHD_GPU_STREAM" "$baseline"; then
    echo "STREAM MATCHES $baseline ($(wc -l < "$baseline") frames, $(awk '{p += $3} END {print p}' "$baseline") packets)"
else
    echo "STREAM DIFFERS from $baseline, first at:"
    diff <(cat "$WWHD_GPU_STREAM") "$baseline" | head -4
    status=1
fi
audio=/wwhd/data/gx2/$which-audio-cemu.txt
if [ ! -f "$audio" ]; then
    echo "no sound baseline $audio: this run's sound ($(wc -l < "$WWHD_AUDIO_HASH") blocks) is in $WWHD_AUDIO_HASH"
elif cmp -s "$WWHD_AUDIO_HASH" "$audio"; then
    echo "SOUND MATCHES $audio ($(wc -l < "$audio") blocks)"
else
    echo "SOUND DIFFERS from $audio, first at:"
    diff <(cat "$WWHD_AUDIO_HASH") "$audio" | head -4
    status=1
fi
exit $status
