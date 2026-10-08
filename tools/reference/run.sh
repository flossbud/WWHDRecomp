#!/usr/bin/env bash
# Start the reference Cemu on a virtual display (Xvfb), rendering on the host GPU.
#   CEMU_BIN      executable (default: the worker's patched build /wwhd/opt/cemu-src/bin/Cemu_release,
#                 else the extracted 2.6 AppImage; build/wwhd/wwhd-null is ours, src/build.sh)
#   CEMU_PORTABLE its portable data dir (default: next to the real binary, .../portable)
#   WWHD_GAME     path to the game (.wua or extracted title dir)
#   DISPLAY    default :99 (an Xvfb is started if none is running there)
#   REF_LOGFLAG   Cemu log category bitmask written to settings (default 0; 2 = GX2 API calls)
#   REF_VIRTUAL_CLOCK=1           deterministic guest time (patched build only)
#   CEMU_HLE_TRACE=out.zst        binary OS-call trace (patched build only; see hle_trace.py)
#   CEMU_HLE_TRACE_FILTER=gx2.    only trace names with these prefixes (comma-separated)
#   CEMU_HLE_TRACE_EXIT_FRAME=N   end the trace and exit Cemu when frame N begins
#   CEMU_INPUT_SCRIPT=route.txt   frame-keyed controller input (patched build; see tools/reference/routes/)
#   CEMU_SWKBD_AUTO=Link          answer the software keyboard (name entry) with this text (default
#                                 Link; CEMU_SWKBD_AUTO= leaves it to the player: our own frontend)
#   CEMU_SHOT_FRAMES=0-900/60     capture TV/pad at these frames into $CEMU_SHOT_DIR (PPM)
#   CEMU_NO_GAMEPAD=1             GamePad reported absent: the project's single-screen, Pro-Controller
#                                 mode (default 1; set CEMU_NO_GAMEPAD= to re-enable the GamePad)
#   REF_GPU=llvmpipe              render in software instead (default: the first GPU, the worker's
#                                 Intel iGPU; Mesa's "sw" WSI copies its frames into Xvfb, which has
#                                 no DRI3 for direct presentation)
#   REF_PIDFILE=path              where to record the emulator's pid (default portable/cemu.pid);
#                                 callers wait on it with `kill -0`
#   REF_FRESH=1                   delete the emulated NAND (portable/mlc01: saves, account) first,
#                                 so every run starts from the same state
#   REF_CPU_MODE=3                Cemu's CPU mode in the game profile (default 0: one host thread, what every
#                                 check needs; 3: one host thread per core, an experiment for real time)
#   REF_SAVE=dir                  start from this save: the game's save files (cking.sav, the
#                                 Pictograph photos cking_pic*.sav, cking_playlog.sav) are copied into
#                                 the default account's save folder, replacing what is there (after
#                                 REF_FRESH). Saves are game data: keep them outside git
#                                 (/wwhd/data/saves on the worker).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
# default: the patched build on the worker, else the extracted AppImage
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
export CEMU_NO_GAMEPAD=${CEMU_NO_GAMEPAD-1}; [ -z "$CEMU_NO_GAMEPAD" ] && unset CEMU_NO_GAMEPAD
export CEMU_HLE_TRACE CEMU_HLE_TRACE_FILTER CEMU_HLE_TRACE_EXIT_FRAME CEMU_INPUT_SCRIPT CEMU_SHOT_FRAMES CEMU_SHOT_DIR
export CEMU_SWKBD_AUTO=${CEMU_SWKBD_AUTO-Link}; [ -z "$CEMU_SWKBD_AUTO" ] && unset CEMU_SWKBD_AUTO
[ -n "${REF_FRESH:-}" ] && rm -rf "$portable/mlc01"
if [ -n "${REF_SAVE:-}" ]; then
    save=$portable/mlc01/usr/save/00050000/10143500/user/80000001   # USA title, Cemu's default account
    ls "$REF_SAVE"/cking.sav >/dev/null
    rm -rf "$save" && mkdir -p "$save" && cp "$REF_SAVE"/*.sav "$save/"
fi
mkdir -p "$portable/controllerProfiles" "$portable/gameProfiles"
sed "s/^cpuMode = 0/cpuMode = ${REF_CPU_MODE:-0}/" "$here/0005000010143500.ini" > "$portable/gameProfiles/0005000010143500.ini"
cp "$here/controller0.xml" "$portable/controllerProfiles/controller0.xml"
sed "s|<logflag>0</logflag>|<logflag>${REF_LOGFLAG:-0}</logflag>|" "$here/settings.xml" \
    > "$portable/settings.xml"  # also skips the first-run wizard

# 2200x1100 fits the TV window (1280x745) and the GamePad window (854x480) side by side.
# openbox gives keyboard focus/activation; without a WM, Cemu ignores key presses.
xdpyinfo >/dev/null 2>&1 || { Xvfb "$DISPLAY" -screen 0 2200x1100x24 -nolisten tcp >/dev/null 2>&1 & sleep 1; }
pgrep -x openbox >/dev/null || { openbox >/dev/null 2>&1 & sleep 1; }
export WWHD_CEMU_DATA=${WWHD_CEMU_DATA:-/wwhd/opt/cemu-src/bin}   # Cemu's resources/ for build/wwhd/wwhd
export MESA_VK_WSI_DEBUG=${MESA_VK_WSI_DEBUG-sw}
[ "${REF_GPU:-}" = llvmpipe ] && export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/tmp/xdg-$(id -u)}; mkdir -p -m 700 "$XDG_RUNTIME_DIR"
pulseaudio --check 2>/dev/null || pulseaudio --start --exit-idle-time=-1
pactl list short sinks | grep -q null || pactl load-module module-null-sink sink_name=null >/dev/null

# One instance per portable dir (they hold the NAND and log); different binaries may run side by side.
pidfile=${REF_PIDFILE:-$portable/cemu.pid}
if [ -f "$pidfile" ] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then
    echo "already running as pid $(cat "$pidfile") ($pidfile)" >&2; exit 1
fi
for attempt in 1 2 3; do
    : > "$portable/log.txt"
    (cd "$(dirname "$bin")" && exec "$bin" -g "$game" >/dev/null 2>&1) &
    echo $! > "$pidfile"
    # Cemu 2.6 sometimes deadlocks in a forked child before logging starts; retry if so.
    for _ in $(seq 60); do grep -q 'Run title' "$portable/log.txt" 2>/dev/null && { echo "cemu running (attempt $attempt)"; exit 0; }; sleep 1; done
    echo "no 'Run title' after 60s, restarting (attempt $attempt)" >&2
    kill "$(cat "$pidfile")" 2>/dev/null || true; sleep 2; kill -9 "$(cat "$pidfile")" 2>/dev/null || true
done
exit 1
