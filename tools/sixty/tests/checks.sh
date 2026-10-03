#!/usr/bin/env bash
# checks.sh NAME: the switch-off checks after a build (they must all match before a commit): both
# routes' OS-call traces, command streams and sound; diff mode on the save route; renderer captures
# (llvmpipe) against the G3 ones (byte-identical). Outputs are per checkout and NAME.
set -uo pipefail
source "$(dirname "$0")/common.sh"
name=$(basename "$ROOT")-${1:?name}
echo "== save"; tools/reference/stream_check.sh save $name-save 2>&1 | tail -4
echo "== route"; tools/reference/stream_check.sh route $name-route 2>&1 | tail -4
echo "== diff"
mkdir -p $OUT/diff
env CEMU_BIN=$ROOT/build/wwhd/wwhd-null WWHD_NATIVE=diff WWHD_RT_LOG=$OUT/diff/rt.log REF_SAVE=/wwhd/data/saves/wwhd_100 \
    tools/reference/route.sh $OUT/diff 1800 tools/reference/routes/continue-100.txt /wwhd/data/traces/save-det/a.zst 2>&1 | tail -2
grep "diff (final)" $OUT/diff/rt.log | tail -1
echo "== captures"
mkdir -p $OUT/bin-survey && cp build/wwhd/wwhd-null $OUT/bin-survey/wwhd-null
rm -rf $OUT/survey-$name
env CEMU_BIN=$OUT/bin-survey/wwhd-null REF_GPU=llvmpipe WWHD_RENDER=vk REF_SAVE=/wwhd/data/saves/wwhd_100 \
    tools/reference/survey.sh tools/reference/routes/continue-100.txt $OUT/survey-$name 900 60 60 2>&1 | tail -1
uv run tools/reference/compare_frames.py /wwhd/data/g3/save-vk $OUT/survey-$name 2>&1 | tail -2
echo done
