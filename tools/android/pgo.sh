#!/usr/bin/env bash
# tools/android/pgo.sh [PROFILE [OUT_DIR]] (m-android A4): the app's library with profile-guided optimisation of the
# generated code, as the desktop's deploys (tools/recomp/pgo.sh: -11% of the game thread on x86). PROFILE (default
# build/pgo/wwhd.profdata) is pgo.sh's, from the worker (x86) or the arm64 worker (WWHD_ON=mac: the same C++, its control
# flow the same but for the barriers, which have none); the NDK's clang (18) can't read the indexed format of the
# workers' LLVM (19), so it goes through the text format: the host's llvm-profdata writes it, the NDK's reads it.
# Then tools/android/build.sh OUT_DIR (default build/android-pgo) with it; apk.sh OUT_DIR/libwwhd-play.so packs it.
# The profile is generated output: it stays in build/, never in git. On the worker, after tools/android/setup.sh.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
profile=${1:-$root/build/pgo/wwhd.profdata}; out=${2:-$root/build/android-pgo}
ndk=${ANDROID_DIR:-/wwhd/opt/android}/ndk/27.2.12479018/toolchains/llvm/prebuilt/linux-x86_64/bin
w=$root/build/pgo-android; mkdir -p "$w"
llvm-profdata merge --text -o "$w/wwhd.proftext" "$profile"
"$ndk/llvm-profdata" merge -o "$w/wwhd.profdata" "$w/wwhd.proftext"
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
