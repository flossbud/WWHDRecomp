#!/usr/bin/env bash
# pgo.sh [OUT_DIR]: wwhd-play with profile-guided optimisation of the generated code (t-codegen 1b, session top). Run on
# the worker after tools/recomp/build.sh: tools/worker/job start pgo tools/recomp/pgo.sh
# 1. build/wwhd-pgogen: wwhd-play's recompiled program instrumented (-fprofile-generate, PLAY_OPT; the rest as always)
# 2. its wwhd-play plays PGO_ROUTES (default save, the continue route, and tour3) at 60 in real time, rendered; each run writes its
#    profile at exit (src/runtime/build_kind.cpp: also at quick_exit, how the routes end)
# 3. build/pgo/wwhd.profdata, merged (llvm-profdata), and OUT_DIR (default build/wwhd-pgo) built with it
#    (-fprofile-use, wwhd-play's copy only): OUT_DIR/wwhd-play. The profile is generated output: it stays in build/, never in git.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
gen=$root/build/wwhd-pgogen; out=${1:-$root/build/wwhd-pgo}; prof=$root/build/pgo
if [ "${PGO_PROFILE:-}" = keep ] && [ -f "$prof/wwhd.profdata" ]; then
    echo "pgo: the last profile kept ($prof/wwhd.profdata)"
else
    rm -rf "$prof"; mkdir -p "$prof"
    # LDFLAGS: the profile runtime at the link (CMake reads it only when it first configures a build directory: one
    # configured without it, or a failed configure, starts again)
    grep -qs "fprofile-generate" "$gen/CMakeCache.txt" || rm -rf "$gen"
    LDFLAGS=-fprofile-generate PLAY_OPT="-fprofile-generate" src/build.sh "$gen" | grep -E "error|wwhd: built"
    for r in ${PGO_ROUTES:-save tour3}; do
        LLVM_PROFILE_FILE="$prof/$r-%p.profraw" WWHD_RENDER=vk SIXTY_REALTIME=1 CEMU_BIN="$gen/wwhd-play" SIXTY_OUT="$prof/out-$r" \
            tools/sixty/run.sh "$r" "$prof/run-$r" 60 > "$prof/run-$r.log" 2>&1 || { echo "pgo: $r failed (see $prof/run-$r.log)"; exit 1; }
    done
    ls "$prof"/*.profraw > /dev/null 2>&1 || { echo "pgo: no profile written"; exit 1; }
    llvm-profdata merge -o "$prof/wwhd.profdata" "$prof"/*.profraw
    echo "pgo: profile from ${PGO_ROUTES:-save tour3}: $(du -h "$prof/wwhd.profdata" | cut -f1)"
fi
# a stale profile (the generated code changed since it was made) can't break the build or change what the program does:
# clang ignores a function's data when its control flow no longer matches, and says so per file; the totals are
# reported here (PGO_PROFILE=keep: build with the last profile, without playing the routes again)
# the profile isn't a dependency ninja sees: wwhd-play's generated code compiles again, so with this profile
rm -rf "$out"/src/CMakeFiles/wwhd_recomp_play.dir
PLAY_OPT="-fprofile-use=$prof/wwhd.profdata;-Wno-profile-instr-unprofiled" src/build.sh "$out" > "$prof/use.log" 2>&1 \
    || { grep -E "error" "$prof/use.log" | head; exit 1; }
grep -E "wwhd: built" "$prof/use.log"
python3 - "$prof/use.log" <<'PY'
import re, sys
t = open(sys.argv[1]).read()
n = len(re.findall(r"control flow change detected \(hash mismatch\)", t))
print(f"pgo: {n} functions' profile out of date (their counts ignored; the code changed since the profile)" if n else
      "pgo: the profile matches the code (no hash mismatches)")
PY
