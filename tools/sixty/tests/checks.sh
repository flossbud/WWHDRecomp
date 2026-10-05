#!/usr/bin/env bash
# checks.sh NAME: the switch-off checks after a build (they must all match before a commit): both
# routes' OS-call traces, command streams and sound; diff mode on the save route; renderer captures
# (llvmpipe) against the G3 ones (byte-identical). Outputs are per checkout and NAME.
# One check run at a time on the worker (the sessions share its CPU): a run waits for the lock
# /wwhd/data/m6/.checks.lock, printing a line a minute (so `job wait` doesn't call it stalled).
# The references (/wwhd/data/gx2/*.txt, traces, g3 captures) are on both workers.
set -uo pipefail
source "$(dirname "$0")/common.sh"
name=$(basename "$ROOT")-${1:?name}
[ -f /wwhd/data/gx2/save-cemu.txt ] || { echo "checks.sh: no references on this worker"; exit 2; }
exec 9>/wwhd/data/m6/.checks.lock
waited=0
until flock -w 60 9; do
    waited=$((waited + 1))
    echo "checks.sh: waiting for the check run of $(cat /wwhd/data/m6/.checks.holder 2>/dev/null || echo another session) (${waited} min)"
done
echo "$name since $(date +%H:%M)" > /wwhd/data/m6/.checks.holder
[ $waited -gt 0 ] && echo "checks.sh: started after waiting ${waited} min"
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
