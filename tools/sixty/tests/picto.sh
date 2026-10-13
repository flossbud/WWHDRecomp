#!/usr/bin/env bash
# picto.sh: the Pictograph Box makes real pictures (B79, session top). Route picto (the 100% save's Quest Log 3, whose
# album has room): a picture taken and kept twice, the album, the game saved. Rendered (llvmpipe, about 10 minutes):
# 1. "Save this pictograph?" (f1340) shows the picture: the viewfinder's middle isn't black;
# 2. the album (f1700) shows the newest one: its middle isn't black;
# 3. the saved game's pictographs (cking_pic2_2 and _3: the JPEGs the game encoded; the run goes on to f2900, the game
#    saved) are 800x450 and not black.
# PASS or FAIL per check; exit status 1 on a FAIL. Run it on the worker: tools/worker/job start picto tools/sixty/tests/picto.sh
set -uo pipefail
source "$(dirname "$0")/common.sh"
# PICTO_RATE=60: the same at 60, its captures at the same game frames (swaps 900 + 2 (f - 900)). With frames dropped
# (WWHD_60FPS_DROPTEST, WWHD_FRAMESKIP_TEST: B77) set PICTO_SCREENS=0: a capture counts presents, and the frame after a
# dropped one presents nothing, so 1 and 2 would look at later moments; the saved JPEGs (3) still say it
rate=${PICTO_RATE:-30}
W=$OUT/picto-$rate-${PICTO_TAG:-plain}; rm -rf "$W"; mkdir -p "$W/bin"     # PICTO_TAG: runs side by side
cp build/wwhd/wwhd-null "$W/bin/" || exit 2
steps=2; [[ ${WWHD_STEPS:-} =~ ^[3-8]$ ]] && steps=$WWHD_STEPS   # (WWHD_STEPS: N steps a tick, U4; as soak.sh reads it)
at() { [ "$rate" = 60 ] && echo $((900 + steps * ($1 - 900))) || echo "$1"; }
f1=$(at 1340) f2=$(at 1700)
SIXTY_FRAMES=2900 CEMU_BIN=$W/bin/wwhd-null SIXTY_OUT=$W tools/sixty/tests/shots.sh picto "$f1,$f2" "$rate" > /dev/null
fail=0
check() {                                      # check WHAT MEAN: PASS above 0.05 (black is 0), else FAIL
    if awk -v m="${2:-0}" 'BEGIN { exit !(m > 0.05) }'; then echo "PASS  $1 (mean $2)"; else echo "FAIL  $1 (mean ${2:-none})"; fail=1; fi
}
# the middle of the picture shown: 1920x1080 captures, the viewfinder's and the album's picture around the centre-left
mean() { [ -f "$1" ] && convert "$1" -crop "$2" -format '%[fx:mean]' info: 2>/dev/null; }
if [ "${PICTO_SCREENS:-1}" != 0 ]; then
    check "the picture in \"Save this pictograph?\"" "$(mean "$W/shots-picto/f$(printf %06d "$f1").tv.ppm" 500x250+700+250)"
    check "the album's newest picture" "$(mean "$W/shots-picto/f$(printf %06d "$f2").tv.ppm" 500x250+700+250)"
fi
save=$W/bin/portable/mlc01/usr/save/00050000/10143500/user/80000001
for i in 2 3; do
    python3 - "$save/cking_pic2_$i.sav" "$W/pic$i.jpg" <<'PY' 2>/dev/null
import sys
d = open(sys.argv[1], 'rb').read()
s, e = d.find(b'\xff\xd8'), d.rfind(b'\xff\xd9')
if s >= 0 and e > s:
    open(sys.argv[2], 'wb').write(d[s:e + 2])
PY
    size=$(identify -format '%wx%h' "$W/pic$i.jpg" 2>/dev/null)
    [ "$size" = 800x450 ] || { echo "FAIL  saved pictograph $i: no 800x450 JPEG (${size:-none})"; fail=1; continue; }
    check "saved pictograph $i" "$(convert "$W/pic$i.jpg" -format '%[fx:mean]' info:)"
done
[ $fail = 0 ] && echo "picto: ok" || echo "picto: FAIL"
exit $fail
