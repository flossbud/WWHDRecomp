#!/usr/bin/env bash
# tools/android/build.sh [OUT_DIR]: libmain.so for the Android app (m-android A2), on the worker after
# tools/android/setup.sh: tools/worker/job start android tools/android/build.sh
# The same CMake project as src/build.sh, built by the NDK's clang for arm64-v8a (API 33) with vcpkg's dependencies
# built for Android (triplet arm64-android-wwhd) from Cemu's manifest less what the app doesn't link (wxWidgets,
# tiff, libusb; dbus is Linux-only there). SDL3 is vcpkg's, its Java side from the same release
# (tools/android/apk.sh). The generated program is the x86 build's (build/recomp: the same C++).
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${1:-$root/build/android}
cemu=${CEMU_SRC:-/wwhd/opt/cemu-src}
sdk=${ANDROID_DIR:-/wwhd/opt/android}
export ANDROID_NDK_HOME=$sdk/ndk/27.2.12479018
export ANDROID_X11_HEADERS=$sdk/x11/include
mkdir -p "$out/manifest"
python3 - "$cemu/vcpkg.json" "$out/manifest/vcpkg.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
drop = {"wxwidgets", "tiff", "libusb", "dbus"}
name = lambda e: e if isinstance(e, str) else e["name"]
d["dependencies"] = [e for e in d["dependencies"] if name(e) not in drop]
d["overrides"] = [e for e in d.get("overrides", []) if e["name"] not in drop]
new = json.dumps(d, indent=2) + "\n"
try:
    old = open(sys.argv[2]).read()
except FileNotFoundError:
    old = None
if old != new:
    open(sys.argv[2], "w").write(new)
PY
args=(-DCEMU_SRC="$cemu" -DWWHD_SDL3= -DWWHD_VCPKG_MANIFEST="$out/manifest" -DWWHD_VCPKG_SYSTEM_PORTS=OFF
      -DCMAKE_MODULE_PATH="$root/tools/android/cmake;$root/tools/arm64/cmake"
      -DCMAKE_PROJECT_INCLUDE="$root/tools/android/cmake/project.cmake"
      # Cemu's desktop extras the app doesn't use (the renderer is ours; no Wiimotes, Wayland or gamemode)
      -DENABLE_OPENGL=OFF -DENABLE_WAYLAND=OFF -DENABLE_BLUEZ=OFF -DENABLE_HIDAPI=OFF -DENABLE_FERAL_GAMEMODE=OFF
      -DENABLE_DISCORD_RPC=OFF -DENABLE_LIBUSB=OFF
      -DWWHD_RECOMP_DIR="${RECOMP_DIR-$root/build/recomp}" -DWWHD_PLAY_BUILD=ON)
[ -f "$out/CMakeCache.txt" ] || args+=(-G Ninja -DCMAKE_BUILD_TYPE=Release
      -DVCPKG_TARGET_TRIPLET=arm64-android-wwhd -DVCPKG_OVERLAY_TRIPLETS="$root/tools/android/triplets"
      -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake"
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33)
cmake -S "$root" -B "$out" "${args[@]}"
cmake --build "$out" -j "${JOBS:-$(cat /wwhd/data/m6/.build.jobs 2>/dev/null || echo 10)}" -- ${NINJA_ARGS:-}   # NINJA_ARGS="-k 0": every error at once
echo "android: built $out/libwwhd-play.so (apk.sh packs it as libmain.so)"
