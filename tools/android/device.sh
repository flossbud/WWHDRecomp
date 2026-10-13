#!/usr/bin/env bash
# tools/android/device.sh CMD (in the worker's container, tools/worker/w): the test device over adb (m-android A3), with
# the SDK's platform-tools (tools/android/setup.sh). Wireless debugging over the tailnet; the device's address is kept
# in /wwhd/opt/android/serial (never in git), or ANDROID_SERIAL.
#   pair HOST:PORT CODE     pair once (the device's "Pair device with pairing code" screen)
#   connect HOST:PORT       connect (the Wireless debugging screen's address; its port changes when it's turned on)
#   install [APK]           install the app (default build/android/windsong.apk), keeping its files
#   files                   the app's files as tools/play/deploy.sh lays them out: Cemu's resources, the settings, the
#                           pause menu's assets, the game (1.5 GB, once). Game data: on the device only.
#   run                     start the app, then the log (Ctrl-C ends the log, not the app)
#   log [SECONDS]           the app's logcat (default: until Ctrl-C)
#   stop                    stop the app
#   uninstall               remove the app and its files (restores the device)
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
sdk=${ANDROID_DIR:-/wwhd/opt/android}
adb=$sdk/platform-tools/adb
pkg=org.windsong.app
cmd=${1:?usage: device.sh pair|connect|install|files|run|log|stop|uninstall}; shift
if [ -z "${ANDROID_SERIAL:-}" ] && [ -f "$sdk/serial" ]; then export ANDROID_SERIAL=$(cat "$sdk/serial"); fi
dir=/sdcard/Android/data/$pkg/files
sh() { "$adb" shell "$@"; }
push_tar() {   # stdin (a tar) unpacked into $dir/$1 on the device
    sh "mkdir -p $dir/$1 && tar -x -C $dir/$1"
}
case "$cmd" in
pair) "$adb" pair "$1" "$2" ;;
connect) "$adb" connect "$1" && echo "$1" > "$sdk/serial" && "$adb" -s "$1" shell getprop ro.product.model ;;
install) "$adb" install -r "${1:-$root/build/android/windsong.apk}" ;;
files)
    sh "mkdir -p $dir/game $dir/cemu $dir/portable/gameProfiles $dir/portable/controllerProfiles $dir/assets"
    sh "test -d $dir/cemu/resources" || tar -c -C /wwhd/opt/cemu-src/bin resources gameProfiles | push_tar cemu
    ref=$root/tools/reference
    "$adb" push "$ref/settings.xml" "$dir/portable/settings.xml" >/dev/null
    "$adb" push "$ref/0005000010143500.ini" "$dir/portable/gameProfiles/0005000010143500.ini" >/dev/null
    "$adb" push "$ref/controller0.xml" "$dir/portable/controllerProfiles/controller0.xml" >/dev/null
    list=$root/config/US_v0/shader_list.txt
    [ -f "$list" ] && sh "mkdir -p $dir/cemu/wwhd" && "$adb" push "$list" "$dir/cemu/wwhd/shader_list.txt" >/dev/null
    tar -c -C "$root/assets" pause_menu | push_tar assets
    if ! sh "test -f $dir/game/wwhd.wua"; then
        echo "copying the game (1.5 GB)"
        "$adb" push /wwhd/data/rom/*.wua "$dir/game/wwhd.wua.part" && sh "mv $dir/game/wwhd.wua.part $dir/game/wwhd.wua"
    fi
    sh "du -sh $dir" ;;
run)
    "$adb" logcat -c
    sh am start -W -n "$pkg/.WindSongActivity"
    exec "$0" log "$@" ;;
log)
    pid=""
    for _ in 1 2 3 4 5 6 7 8 9 10; do pid=$(sh pidof "$pkg" | tr -d '\r') && [ -n "$pid" ] && break; sleep 1; done
    args=(-v time); [ -n "$pid" ] && args+=(--pid="$pid") || echo "device.sh: $pkg isn't running; the whole log" >&2
    if [ -n "${1:-}" ]; then timeout "$1" "$adb" logcat "${args[@]}" || true; else "$adb" logcat "${args[@]}"; fi ;;
stop) sh am force-stop "$pkg" ;;
uninstall) "$adb" uninstall "$pkg" ;;
*) echo "device.sh: unknown command $cmd" >&2; exit 2 ;;
esac
