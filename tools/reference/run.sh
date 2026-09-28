#!/usr/bin/env bash
# Start the reference Cemu on a virtual display with software Vulkan.
#   CEMU_BIN      executable (default: the extracted 2.6 AppImage, ~/opt/cemu/squashfs-root/AppRun;
#                 the patched source build is ~/opt/cemu-src/bin/Cemu_release)
#   CEMU_PORTABLE its portable data dir (default: next to the real binary, .../portable)
#   WWHD_GAME     path to the game (.wua or extracted title dir)
#   DISPLAY    default :99 (an Xvfb is started if none is running there)
#   REF_LOGFLAG   Cemu log category bitmask written to settings (default 0; 2 = GX2 API calls)
#   REF_VIRTUAL_CLOCK=1           deterministic guest time (patched build only)
#   CEMU_HLE_TRACE=out.zst        binary OS-call trace (patched build only; see hle_trace.py)
#   CEMU_HLE_TRACE_FILTER=gx2.    only trace names with these prefixes (comma-separated)
#   CEMU_HLE_TRACE_EXIT_FRAME=N   end the trace and exit Cemu when frame N begins
#   REF_FRESH=1                   delete the emulated NAND (portable/mlc01: saves, account) first,
#                                 so every run starts from the same state
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
# default: the patched build on the worker worker, else the extracted AppImage
if [ -z "${CEMU_BIN:-}" ] && [ -x /wwhd/opt/cemu-src/bin/Cemu_release ]; then CEMU_BIN=/wwhd/opt/cemu-src/bin/Cemu_release; fi
bin=${CEMU_BIN:-$HOME/opt/cemu/squashfs-root/AppRun}
case "$bin" in
    */AppRun) portable=${CEMU_PORTABLE:-$(dirname "$bin")/usr/bin/portable} ;;
    *)        portable=${CEMU_PORTABLE:-$(dirname "$bin")/portable} ;;
esac
if [ -z "${WWHD_GAME:-}" ]; then for f in /wwhd/data/rom/*.wua; do [ -f "$f" ] && WWHD_GAME=$f; done; fi
game=${WWHD_GAME:?set WWHD_GAME to your .wua or title directory}
export DISPLAY=${DISPLAY:-:99}
[ -n "${REF_VIRTUAL_CLOCK:-}" ] && export CEMU_VIRTUAL_CLOCK=1
export CEMU_HLE_TRACE CEMU_HLE_TRACE_FILTER CEMU_HLE_TRACE_EXIT_FRAME
[ -n "${REF_FRESH:-}" ] && rm -rf "$portable/mlc01"
mkdir -p "$portable/controllerProfiles" "$portable/gameProfiles"
cp "$here/0005000010143500.ini" "$portable/gameProfiles/"
cp "$here/controller0.xml" "$portable/controllerProfiles/controller0.xml"
sed "s|<logflag>0</logflag>|<logflag>${REF_LOGFLAG:-0}</logflag>|" "$here/settings.xml" \
    > "$portable/settings.xml"  # also skips the first-run wizard

# 2200x1100 fits the TV window (1280x745) and the GamePad window (854x480) side by side.
# openbox gives keyboard focus/activation; without a WM, Cemu ignores key presses.
xdpyinfo >/dev/null 2>&1 || { Xvfb "$DISPLAY" -screen 0 2200x1100x24 -nolisten tcp >/dev/null 2>&1 & sleep 1; }
pgrep -x openbox >/dev/null || { openbox >/dev/null 2>&1 & sleep 1; }
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/tmp/xdg-$(id -u)}; mkdir -p -m 700 "$XDG_RUNTIME_DIR"
pulseaudio --check 2>/dev/null || pulseaudio --start --exit-idle-time=-1
pactl list short sinks | grep -q null || pactl load-module module-null-sink sink_name=null >/dev/null

pgrep -x cemu >/dev/null && { echo "cemu already running" >&2; exit 1; }
for attempt in 1 2 3; do
    : > "$portable/log.txt"
    (cd "$(dirname "$bin")" && "$bin" -g "$game" >/dev/null 2>&1 &)
    # Cemu 2.6 sometimes deadlocks in a forked child before logging starts; retry if so.
    for _ in $(seq 60); do grep -q 'Run title' "$portable/log.txt" 2>/dev/null && { echo "cemu running (attempt $attempt)"; exit 0; }; sleep 1; done
    echo "no 'Run title' after 60s, restarting (attempt $attempt)" >&2
    pkill -x cemu || true; sleep 2; pkill -9 -x cemu || true
done
exit 1
