#!/usr/bin/env bash
# play.sh [ARGS...]  - The Wind Waker HD, recompiled, in a window on this machine: our renderer on
# Vulkan, sound and input through SDL3, guest time from the host clock (real time). Runs in the
# directory tools/play/deploy.sh fills: wwhd-null, cemu/ (Cemu's resources and game profiles),
# portable/ (settings, and the emulated NAND with your saves), game/wwhd.wua, saves/, routes/.
#   WWHD_SAVE=dir     first install the save files in dir (cking*.sav), replacing the current save
#                     (e.g. WWHD_SAVE=saves/wwhd_100: Quest Log 1 is a 100% file)
#   WWHD_VSYNC=1      present with FIFO (vsync) instead of mailbox
#   WWHD_WINDOW=0     headless: no window and no sound (the sound is hashed into /dev/null), for
#                     tests with WWHD_EXIT_FRAME
# Other switches pass through (src/README.md): WWHD_EXIT_FRAME, CEMU_INPUT_SCRIPT, CEMU_SHOT_FRAMES,
# CEMU_VIRTUAL_CLOCK, WWHD_PROFILE. Keys (the Pro Controller): A=X B=Z X=S Y=A L=Q R=W ZL=1 ZR=2
# +=Return -=Backspace, D-pad on the arrows, left stick I/J/K/L, right stick T/F/G/H; F11 or
# Alt+Enter toggles fullscreen. F1 (or both sticks) opens the debug menu: arrows move, X or Enter
# chooses, Z goes back, Escape or F1 closes; the mouse too: hover, left click chooses, right click goes
# back, the wheel moves, a click outside closes. F2 opens the settings (frame rate, vsync, display, CPU
# threads...; saved in portable/wwhd.ini; a switch set here in the environment wins over it). A gamepad's
# buttons go by their printed labels.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
export WWHD_NATIVE=${WWHD_NATIVE-on} WWHD_RENDER=${WWHD_RENDER-vk} WWHD_CEMU_DATA=$here/cemu CEMU_NO_GAMEPAD=1
export WWHD_WINDOW=${WWHD_WINDOW-1}
[ "$WWHD_WINDOW" = 0 ] && { unset WWHD_WINDOW; export WWHD_AUDIO_HASH=${WWHD_AUDIO_HASH:-/dev/null}; }
if [ -n "${WWHD_SAVE:-}" ]; then
    save=$here/portable/mlc01/usr/save/00050000/10143500/user/80000001   # USA title, the default account
    ls "$WWHD_SAVE"/cking.sav >/dev/null
    rm -rf "$save" && mkdir -p "$save" && cp "$WWHD_SAVE"/*.sav "$save/"
fi
cd "$here" && exec ./wwhd-null -g "$here/game/wwhd.wua" "$@"
