#!/usr/bin/env bash
# build.sh [OUT_DIR]  - configure and build wwhd-null with CMake (CMakeLists.txt, src/CMakeLists.txt).
# Run on the worker: tools/worker/job start wwhd-build src/build.sh
#   OUT_DIR/wwhd-null (default build/wwhd): Cemu's OS libraries without Latte, with our forks of some
#   of their sources (src/forks.txt); the null GPU (src/gpu/null_gpu.cpp) and our Vulkan renderer;
#   our OS layer and frontend; the runtime (src/runtime) and, when it has been generated, the
#   recompiled program with its overrides. Headless, or in a window with SDL3.
# The first build compiles Cemu's libraries from CEMU_SRC too (11-12 minutes with 10 jobs); later ones
# compile what changed and link twice (the second time in the reference's member order,
# src/link_order.py), about 10 s for a one-file change thanks to ThinLTO's cache.
# Env: CEMU_SRC (default /wwhd/opt/cemu-src: the pinned Cemu with tools/reference/cemu-patches);
#   SDL3_DIR (default /wwhd/opt/sdl3: SDL3 with video, tools/worker/setup-volume.sh sdl3);
#   RECOMP_DIR: the generated program (tools/recomp/build.sh; default build/recomp, RECOMP_DIR= for
#   none: the runtime then only interprets); WWHD_FORKS=0: Cemu's objects instead of our forks, to
#   record baselines (WWHD_FORKS=0 src/build.sh build/wwhd-cemu); JOBS (default 10, the container's
#   CPU quota).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$root/build/wwhd}
args=(-DCEMU_SRC="${CEMU_SRC:-/wwhd/opt/cemu-src}" -DWWHD_SDL3="${SDL3_DIR:-/wwhd/opt/sdl3}"
      -DWWHD_RECOMP_DIR="${RECOMP_DIR-$root/build/recomp}" -DWWHD_FORKS="$([ "${WWHD_FORKS:-1}" = 0 ] && echo OFF || echo ON)")
[ -f "$out/CMakeCache.txt" ] || args+=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++)
cmake -S "$root" -B "$out" "${args[@]}"
cmake --build "$out" -j "${JOBS:-10}"
echo "wwhd: built $out/wwhd-null"
