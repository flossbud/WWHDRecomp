#!/usr/bin/env bash
# build.sh [OUT_DIR]  - build the wwhd executable against the worker's Cemu build (M0b.1).
# Run on the worker: tools/worker/job start wwhd-build src/build.sh  -> build/wwhd/wwhd
# Our sources compile with Cemu's own flags (tools/cemu_flags.py). The link line is Cemu_release's,
# with Cemu's wxWidgets GUI library replaced by our frontend (src/frontend), which implements
# Cemu's WindowSystem interface; Cemu's main.cpp stays and calls WindowSystem::Create().
# Env: CEMU_SRC (default /wwhd/opt/cemu-src).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cemu=${CEMU_SRC:-/wwhd/opt/cemu-src}
out=$(mkdir -p "${1:-$root/build/wwhd}" && cd "${1:-$root/build/wwhd}" && pwd)

mapfile -t flags < <(python3 "$root/tools/cemu_flags.py" "$cemu/build/compile_commands.json" Espresso/Interpreter/PPCInterpreterFPU.cpp)
cxx=(clang++ "${flags[@]}" -fno-lto -include "$cemu/src/Common/precompiled.h" -I"$root/src/frontend" -O2)
objs=()
pids=()
for src in "$root"/src/frontend/*.cpp; do
    obj="$out/$(basename "${src%.cpp}").o"
    "${cxx[@]}" -c "$src" -o "$obj" & pids+=($!)
    objs+=("$obj")
done
for p in "${pids[@]}"; do wait "$p"; done

echo "wwhd: linking"
cd "$cemu/build"
link=$(ninja -t commands "$cemu/bin/Cemu_release" | tail -1)
link=${link#": && "}; link=${link%" && :"}
link=${link//" src/gui/wxgui/libCemuWxGui.a"/}
link=${link/"-o $cemu/bin/Cemu_release"/"-o $out/wwhd ${objs[*]}"}
link=${link/"-Xlinker --dependency-file=src/CMakeFiles/CemuBin.dir/link.d"/}
eval "$link"
echo "wwhd: built $out/wwhd"
