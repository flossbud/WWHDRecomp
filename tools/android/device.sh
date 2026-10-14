#!/usr/bin/env bash
# tools/android/device.sh CMD (in the worker's container, tools/worker/w): the test device over adb (m-android A3), with
# the SDK's platform-tools (tools/android/setup.sh). Wireless debugging over the tailnet; the device's address is kept
# in /wwhd/opt/android/serial (never in git), or ANDROID_SERIAL.
#   pair HOST:PORT CODE     pair once (the device's "Pair device with pairing code" screen)
#   connect HOST:PORT       connect (the Wireless debugging screen's address; its port changes when it's turned on)
#   install [APK]           install the app (default build/android/windsong.apk), keeping its files
#   files                   the game for tests (1.5 GB, once; game data: on the device only), in place of Setup's
#                           copy (t-startscreen S3). The app's own files come from its APK (WindSongActivity)
#   run [SECONDS]           start the app, then the log (Ctrl-C ends the log, not the app); ENV="A=1;B=2" sets
#                           environment variables in it
#   log [SECONDS]           the app's logcat (default: until Ctrl-C)
#   stop                    stop the app
#   perf [SECONDS] [LIB]    a CPU profile of the running app (m-android A4): the NDK's simpleperf on the device
#                           (--app: the debuggable APK), SECONDS (default 30) at 4000 Hz with frame-pointer call
#                           graphs, pulled to build/android/perf/perf.data and reported per thread against LIB (default
#                           build/android/libwwhd-play.so, the unstripped library the APK's libmain.so came from):
#                           build/android/perf/report.txt (the threads, then the scheduler thread's own functions)
#   uninstall               remove the app and its files (restores the device)
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
sdk=${ANDROID_DIR:-/wwhd/opt/android}
adb=$sdk/platform-tools/adb
pkg=org.windsong.app
cmd=${1:?usage: device.sh pair|connect|install|files|run|log|stop|perf|uninstall}; shift
if [ -z "${ANDROID_SERIAL:-}" ] && [ -f "$sdk/serial" ]; then export ANDROID_SERIAL=$(cat "$sdk/serial"); fi
dir=/sdcard/Android/data/$pkg/files
sh() { "$adb" shell "$@"; }
case "$cmd" in
pair) "$adb" pair "$1" "$2" ;;
perf)
    secs=${1:-30}; lib=${2:-$root/build/android/libwwhd-play.so}
    sp=$sdk/ndk/27.2.12479018/simpleperf; out=$root/build/android/perf; rm -rf "$out"; mkdir -p "$out/symfs"
    "$adb" push "$sp/bin/android/arm64/simpleperf" /data/local/tmp/simpleperf >/dev/null
    sh "chmod 755 /data/local/tmp/simpleperf"
    sh "/data/local/tmp/simpleperf record --app $pkg --duration $secs -f 4000 --call-graph fp -o /data/local/tmp/perf.data"
    "$adb" pull /data/local/tmp/perf.data "$out/perf.data" >/dev/null
    # the library as the device maps it (its path in the APK's install dir), for the symbols
    path=$(sh "pm path $pkg" | sed 's/^package://; s#/base.apk##' | tr -d '\r')
    mkdir -p "$out/symfs$path/lib/arm64" && cp "$lib" "$out/symfs$path/lib/arm64/libmain.so"
    rep="$sp/bin/linux/x86_64/simpleperf report -i $out/perf.data --symfs $out/symfs"
    { echo "== by thread"; $rep --sort tid,comm 2>/dev/null | head -30
      echo "== the scheduler thread (core 1), by function"; $rep --comms "OSSched[core=1]" --sort symbol 2>/dev/null | head -60
      echo "== all threads, by library"; $rep --sort dso 2>/dev/null | head -20; } > "$out/report.txt"
    cat "$out/report.txt" ;;
connect) "$adb" connect "$1" && echo "$1" > "$sdk/serial" && "$adb" -s "$1" shell getprop ro.product.model ;;
install) "$adb" install -r "${1:-$root/build/android/windsong.apk}" ;;
files)
    sh "mkdir -p $dir/game"
    if ! sh "test -f $dir/game/wwhd.wua"; then
        echo "copying the game (1.5 GB)"
        "$adb" push /wwhd/data/rom/*.wua "$dir/game/wwhd.wua.part" && sh "mv $dir/game/wwhd.wua.part $dir/game/wwhd.wua"
    fi
    sh "du -sh $dir" ;;
run)
    "$adb" logcat -c
    sh input keyevent KEYCODE_WAKEUP      # a sleeping screen pauses the app at once
    sh am start -S -W -f 0x10008000 -n "$pkg/.WindSongActivity" ${ENV:+--es env "'$ENV'"}   # a new task: ENV always applies
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
