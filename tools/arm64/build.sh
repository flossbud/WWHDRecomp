#!/usr/bin/env bash
# tools/arm64/build.sh [OUT_DIR]: wwhd-null for arm64 Linux (m-android A1), on the worker after tools/arm64/setup.sh:
#   tools/worker/job start arm64 tools/arm64/build.sh
# The same CMake project as src/build.sh, cross-compiled by clang (tools/arm64/toolchain.cmake) with vcpkg's
# dependencies built for arm64 (triplet arm64-linux-cross) from Cemu's manifest less what wwhd-null doesn't link
# (wxWidgets, tiff, libusb, dbus). Without a display: SDL3 from vcpkg. The generated program is the x86 build's
# (build/recomp: the same C++); RECOMP_DIR= for none. Run it with qemu-user (tools/arm64/run.sh).
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${1:-$root/build/arm64}
cemu=${CEMU_SRC:-/wwhd/opt/cemu-src}
mkdir -p "$out/manifest"
# pkg-config: the sysroot's .pc files only, never the host's
sysroot=${ARM64_SYSROOT:-/wwhd/opt/arm64/root}
export PKG_CONFIG_SYSROOT_DIR=$sysroot
export PKG_CONFIG_LIBDIR=$sysroot/usr/lib/aarch64-linux-gnu/pkgconfig:$sysroot/usr/share/pkgconfig
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
      -DCMAKE_MODULE_PATH="$root/tools/arm64/cmake"
      # Cemu's desktop extras wwhd-null doesn't use (the renderer is ours; no Wiimotes, Wayland or gamemode)
      -DENABLE_OPENGL=OFF -DENABLE_WAYLAND=OFF -DENABLE_BLUEZ=OFF -DENABLE_HIDAPI=OFF -DENABLE_FERAL_GAMEMODE=OFF
      -DENABLE_DISCORD_RPC=OFF -DENABLE_LIBUSB=OFF
      -DWWHD_RECOMP_DIR="${RECOMP_DIR-$root/build/recomp}" -DWWHD_PLAY_BUILD=OFF)
[ -f "$out/CMakeCache.txt" ] || args+=(-G Ninja -DCMAKE_BUILD_TYPE=Release
      -DVCPKG_TARGET_TRIPLET=arm64-linux-cross -DVCPKG_OVERLAY_TRIPLETS="$root/tools/arm64/triplets"
      -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE="$root/tools/arm64/toolchain.cmake")
cmake -S "$root" -B "$out" "${args[@]}"
cmake --build "$out" -j "${JOBS:-$(cat /wwhd/data/m6/.build.jobs 2>/dev/null || echo 10)}" -- ${NINJA_ARGS:-}   # NINJA_ARGS="-k 0": every error at once
echo "arm64: built $out/wwhd-null"
