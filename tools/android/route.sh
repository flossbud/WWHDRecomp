#!/usr/bin/env bash
# tools/android/route.sh ROUTE [SECONDS] (in the worker's container, tools/worker/w; m-android A4): a scripted route
# (tools/reference/routes.sh) played in real time at 60 on the device, to see the game's speed in play rather than at
# the title. It runs in a test home of its own (WWHD_HOME=files/test-home: the route's save, the reference profiles and
# a new player's settings: no wwhd.ini), so the player's saves, settings and imported drivers are never read or
# written; the test home is deleted at the end. Prints the run's real-time lines (fps, frame times, how busy the threads were). The app
# must be installed and the game in place (device.sh files); the device's owner not using it.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
sdk=${ANDROID_DIR:-/wwhd/opt/android}
adb=$sdk/platform-tools/adb
pkg=org.windsong.app
if [ -z "${ANDROID_SERIAL:-}" ] && [ -f "$sdk/serial" ]; then export ANDROID_SERIAL=$(cat "$sdk/serial"); fi
which=${1:?usage: route.sh ROUTE [SECONDS]}; secs=${2:-}
ref=$root/tools/reference
. "$ref/routes.sh"
route_info "$which" || { echo "routes: $route_names" >&2; exit 2; }
home=/sdcard/Android/data/$pkg/files/test-home
save_dir=$home/portable/mlc01/usr/save/00050000/10143500/user/80000001   # USA title, Cemu's default account
sh() { "$adb" shell "$@" </dev/null; }
cleanup() { sh "am force-stop $pkg"; sh "rm -rf $home"; }
trap cleanup EXIT
sh "rm -rf $home && mkdir -p $home/portable/gameProfiles $home/portable/controllerProfiles $save_dir"
"$adb" push "$ref/settings.xml" "$home/portable/settings.xml" </dev/null >/dev/null
"$adb" push "$ref/0005000010143500.ini" "$home/portable/gameProfiles/0005000010143500.ini" </dev/null >/dev/null
"$adb" push "$ref/controller0.xml" "$home/portable/controllerProfiles/controller0.xml" </dev/null >/dev/null
"$adb" push "$ref/routes/$route" "$home/route.txt" </dev/null >/dev/null
[ -n "$save" ] && for f in "$save"/*.sav; do "$adb" push "$f" "$save_dir/" </dev/null >/dev/null; done
# the run as run.sh's real-time one at 60 (SIXTY_REALTIME=1): the route's own #env lines, the logo straight to the
# file select, 60 from game frame 900, the end two swaps a game frame after that
from=900; exit_frame=$((from + 2 * (frames - from)))
env="WWHD_HOME=$home;CEMU_INPUT_SCRIPT=$home/route.txt;WWHD_START_SCREEN=0;WWHD_DEBUG_BOOT=1;WWHD_60FPS=1"
env="$env;WWHD_60FPS_FROM=$from;WWHD_EXIT_FRAME=$exit_frame"
while IFS= read -r line; do env="$env;${line#\#env }"; done < <(grep '^#env [A-Z_0-9]*=' "$ref/routes/$route" || true)
echo "route.sh: $which ($route, $frames game frames: exit at swap $exit_frame) on the device"
sh "logcat -c"
sh "input keyevent KEYCODE_WAKEUP"
sh "am start -W -n $pkg/.WindSongActivity --es env '$env'" >/dev/null
start=$(date +%s)
for _ in $(seq 1 "${secs:-$((exit_frame / 40 + 60))}"); do
    sleep 1
    [ -n "$(sh "pidof $pkg" | tr -d '\r')" ] || break
done
echo "route.sh: $(( $(date +%s) - start )) s"
sh "cat $home/portable/log.txt" | grep -E "wwhd real time|Fatal|exit frame|FATAL" | cut -c1-220 || true
