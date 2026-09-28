#!/usr/bin/env bash
# shot.sh OUT_PREFIX  - save the TV (1280x720) and GamePad (854x480) render areas as
# OUT_PREFIX.tv.png / OUT_PREFIX.pad.png. Captures Cemu's render child windows (found by size),
# after placing the two top-level windows side by side so nothing overlaps them.
set -euo pipefail
export DISPLAY=${DISPLAY:-:99}
tv=$(xdotool search --onlyvisible --name '^Cemu' | head -1)
pad=$(xdotool search --onlyvisible --name '^GamePad View' | head -1 || true)
xdotool windowmove "$tv" 0 0
[ -n "$pad" ] && xdotool windowmove "$pad" 1300 0
sleep 0.3
child() { xwininfo -tree -id "$1" | awk -v s="$2" '$0 ~ " "s"\\+" {print $1; exit}'; }
import -window "$(child "$tv" 1280x720)" "$1.tv.png"
[ -n "$pad" ] && import -window "$(child "$pad" 854x480)" "$1.pad.png"
xdotool getwindowname "$tv"
