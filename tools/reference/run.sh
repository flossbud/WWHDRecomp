#!/usr/bin/env bash
# Start the reference Cemu (unmodified upstream) on a virtual display with software Vulkan.
#   CEMU_DIR   extracted AppImage (default ~/opt/cemu/squashfs-root), portable mode in usr/bin/portable
#   WWHD_GAME  path to the game (.wua or extracted title dir)
#   DISPLAY    default :99 (an Xvfb is started if none is running there)
#   REF_LOGFLAG Cemu log category bitmask written to settings (default 0; 2 = GX2 API calls)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
cemu=${CEMU_DIR:-$HOME/opt/cemu/squashfs-root}
game=${WWHD_GAME:?set WWHD_GAME to your .wua or title directory}
export DISPLAY=${DISPLAY:-:99}
portable=$cemu/usr/bin/portable
mkdir -p "$portable/controllerProfiles"
cp "$here/controller0.xml" "$portable/controllerProfiles/controller0.xml"
sed "s|<logflag>0</logflag>|<logflag>${REF_LOGFLAG:-0}</logflag>|" "$here/settings.xml" \
    > "$portable/settings.xml"  # also skips the first-run wizard

# 2200x1100 fits the TV window (1280x745) and the GamePad window (854x480) side by side.
# openbox gives keyboard focus/activation; without a WM, Cemu ignores key presses.
xdpyinfo >/dev/null 2>&1 || { Xvfb "$DISPLAY" -screen 0 2200x1100x24 -nolisten tcp >/dev/null 2>&1 & sleep 1; }
pgrep -x openbox >/dev/null || { openbox >/dev/null 2>&1 & sleep 1; }
pulseaudio --check 2>/dev/null || pulseaudio --start --exit-idle-time=-1
pactl list short sinks | grep -q null || pactl load-module module-null-sink sink_name=null >/dev/null

pgrep -x cemu >/dev/null && { echo "cemu already running" >&2; exit 1; }
for attempt in 1 2 3; do
    : > "$portable/log.txt"
    (cd "$cemu" && ./AppRun -g "$game" >/dev/null 2>&1 &)
    # Cemu 2.6 sometimes deadlocks in a forked child before logging starts; retry if so.
    for _ in $(seq 60); do grep -q 'Run title' "$portable/log.txt" 2>/dev/null && { echo "cemu running (attempt $attempt)"; exit 0; }; sleep 1; done
    echo "no 'Run title' after 60s, restarting (attempt $attempt)" >&2
    pkill -x cemu || true; sleep 2; pkill -9 -x cemu || true
done
exit 1
