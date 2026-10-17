#!/usr/bin/env bash
# tools/android/pgo.sh [PROFILE [OUT_DIR]] (m-android A4): the app's library with profile-guided optimisation of the
# generated code, as the desktop's deploys (tools/recomp/pgo.sh: -11% of the game thread on x86). Then
# tools/android/build.sh OUT_DIR (default build/android-pgo) with it; apk.sh OUT_DIR/libwwhd-play.so packs it.
# PROFILE: a directory of .profraw files from the devices (below: the NDK's llvm-profdata merges them, equal weights), or
# a .profdata (default build/pgo/wwhd.profdata) from the workers' pgo.sh, which the NDK's clang (18) can't read in the
# workers' LLVM's (19) indexed format, so through the text format. A workers' profile fits the Android library badly
# (a4-pgo, 2026-10-16: 27,430 of 40,298 functions' control flow differed, 19 of the RP4's 20 hottest: the stores read
# the store table through the context there, ppc_ops.h's WWHD_RT_CTX_TABLE, and another clang): profile on the device.
#
# tools/android/pgo.sh generate [OUT_DIR] (default build/android-pgogen): the library instrumented
# (-fprofile-generate, the generated code only; the profile runtime at the link), packed as OUT_DIR/windsong-pgogen.apk.
# Installed on a device and played (route.sh, the routes in real time: slow, but they go by game frame), each run writes
# its profile at its exit to WWHD_PROFILE_FILE (src/runtime/build_kind.cpp; e.g. beside the test home, %p the process),
# pulled from there into a directory for PROFILE. Profiles are generated output: they stay in build/ and /wwhd/tmp,
# never in git. On the worker, after tools/android/setup.sh.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
ndk=${ANDROID_DIR:-/wwhd/opt/android}/ndk/27.2.12479018/toolchains/llvm/prebuilt/linux-x86_64/bin
if [ "${1:-}" = generate ]; then
    out=${2:-$root/build/android-pgogen}
    # LINK_OPT: the profile runtime at the link
    LINK_OPT=-fprofile-generate PLAY_OPT="-fprofile-generate" tools/android/build.sh "$out" > "$out.log" 2>&1 ||
        { grep -E "error" "$out.log" | head; exit 1; }
    tools/android/apk.sh "$out/libwwhd-play.so" "$out/windsong-pgogen.apk" | tail -1
    exit 0
fi
profile=${1:-$root/build/pgo/wwhd.profdata}; out=${2:-$root/build/android-pgo}
w=$root/build/pgo-android; mkdir -p "$w"
if [ -d "$profile" ]; then
    "$ndk/llvm-profdata" merge -o "$w/wwhd.profdata" "$profile"/*.profraw
else
    llvm-profdata merge --text -o "$w/wwhd.proftext" "$profile"
    "$ndk/llvm-profdata" merge -o "$w/wwhd.profdata" "$w/wwhd.proftext"
fi
echo "android pgo: $("$ndk/llvm-profdata" show "$w/wwhd.profdata" | grep -m1 'Total functions')"
# the generated code compiles again with it (the profile isn't a dependency ninja sees)
rm -rf "$out"/src/CMakeFiles/wwhd_recomp_play.dir
PLAY_OPT="-fprofile-use=$w/wwhd.profdata;-Wno-profile-instr-unprofiled" \
    tools/android/build.sh "$out" > "$w/use.log" 2>&1 || { grep -E "error" "$w/use.log" | head; exit 1; }
grep -E "android: built" "$w/use.log"
python3 - "$w/use.log" <<'PY'
import re, sys
t = open(sys.argv[1]).read()
print(f"android pgo: {len(re.findall(r'control flow change detected', t))} functions' profile out of date (hash mismatch: ignored)")
PY
