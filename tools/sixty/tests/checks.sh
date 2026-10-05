#!/usr/bin/env bash
# checks.sh NAME: the switch-off checks after a build (they must all match before a commit): both
# routes' OS-call traces, command streams and sound; diff mode on the save route; renderer captures
# (llvmpipe) against the G3 ones (byte-identical). Outputs are per checkout and NAME.
# A worker takes a set number of check runs at once (the sessions share its CPU): a run waits for a
# free slot, printing a line a minute (so `job wait` doesn't call it stalled).
# The references (/wwhd/data/gx2/*.txt, traces, g3 captures) are on both workers.
set -uo pipefail
source "$(dirname "$0")/common.sh"
name=$(basename "$ROOT")-${1:?name}
[ -f /wwhd/data/gx2/save-cemu.txt ] || { echo "checks.sh: no references on this worker"; exit 2; }
# slots: how many check runs this worker takes at once (/wwhd/data/m6/.checks.slots, default 1:
# the worker; the desktop's file says 3). Slot 0's lock is .checks.lock, so older copies of this
# script still wait for it.
slots=$(cat /wwhd/data/m6/.checks.slots 2>/dev/null || echo 1)
waited=0
while :; do
    got=
    for ((i = 0; i < slots; i++)); do
        lock=/wwhd/data/m6/.checks.lock; [ $i -gt 0 ] && lock=$lock.$i
        exec 9>"$lock"
        if flock -n 9; then got=$i; break; fi
        exec 9>&-
    done
    [ -n "$got" ] && break
    waited=$((waited + 1))
    echo "checks.sh: waiting for a check slot ($slots here: $(cat /wwhd/data/m6/.checks.holder* 2>/dev/null | tr '\n' ';')) (${waited} min)"
    sleep 60
done
holder=/wwhd/data/m6/.checks.holder; [ "$got" -gt 0 ] && holder=$holder.$got
echo "$name since $(date +%H:%M)" > "$holder"
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
