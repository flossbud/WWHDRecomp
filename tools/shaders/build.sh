#!/usr/bin/env bash
# build.sh [OUT_DIR] - build the G1 shader translator (translate.cpp) against the worker's Cemu build:
#   tools/worker/job start shaders-build tools/shaders/build.sh
# Like the M1 fuzzer, it links into Cemu_release's own link line (-Wl,--wrap=main), so Cemu's shader
# decompiler and glslang are exactly the reference's. Output: OUT_DIR/translate (default build/shaders).
# Env: CEMU_SRC (default /wwhd/opt/cemu-src).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
cemu=${CEMU_SRC:-/wwhd/opt/cemu-src}
out=$(mkdir -p "${1:-$root/build/shaders}" && cd "${1:-$root/build/shaders}" && pwd)

# Cemu's flags for its Vulkan shader compiler (the glslang include paths come with them)
mapfile -t flags < <(python3 "$root/tools/cemu_flags.py" "$cemu/build/compile_commands.json" Latte/Renderer/Vulkan/RendererShaderVk.cpp)
echo "shaders: compiling"
clang++ "${flags[@]}" -fno-lto -include "$cemu/src/Common/precompiled.h" -O2 -c "$here/translate.cpp" -o "$out/translate.o"

echo "shaders: linking"
limits=$(ls "$cemu"/build/vcpkg_installed/*/lib/libglslang-default-resource-limits.a | head -1)
cd "$cemu/build"
link=$(ninja -t commands "$cemu/bin/Cemu_release" | tail -1)
link=${link#": && "}; link=${link%" && :"}
link=${link/"-o $cemu/bin/Cemu_release"/"-o $out/translate $out/translate.o $limits -Wl,--wrap=main"}
link=${link/"-Xlinker --dependency-file=src/CMakeFiles/CemuBin.dir/link.d"/}
eval "$link"
echo "shaders: built $out/translate"
