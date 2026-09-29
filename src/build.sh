#!/usr/bin/env bash
# build.sh [OUT_DIR]  - build the wwhd executables against the worker's Cemu build.
# Run on the worker: tools/worker/job start wwhd-build src/build.sh
#   build/wwhd/wwhd       Cemu's OS libraries + Latte on Vulkan + our frontend (M0b.1)
#   build/wwhd/wwhd-null  the same without Latte: src/gpu/null_gpu.cpp consumes gx2's command
#                         buffers and draws nothing; headless (M0b.2). It also carries the runtime
#                         (src/runtime: the execution seam, rt_*, diff mode) and, when built, the
#                         recompiled program (M3)
# Our sources compile with Cemu's own flags (tools/cemu_flags.py). The link line is Cemu_release's
# with Cemu's wxWidgets GUI library replaced by our frontend, which implements Cemu's
# WindowSystem interface (Cemu's main.cpp stays and calls WindowSystem::Create()). For wwhd-null,
# libCemuCafe.a is copied without the objects built from src/Cafe/HW/Latte (the address library
# in HW/Latte/LatteAddrLib stays: gx2 computes surface layouts with it).
# Env: CEMU_SRC (default /wwhd/opt/cemu-src); RECOMP_DIR: the compiled generated code to link into
# wwhd-null (tools/recomp/build.sh; default build/recomp if it has been built there, RECOMP_DIR=
# for none: the runtime then only interprets).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cemu=${CEMU_SRC:-/wwhd/opt/cemu-src}
recomp=${RECOMP_DIR-$root/build/recomp}
[ -n "$recomp" ] && [ ! -f "$recomp/func_table.o" ] && recomp=
out=$(mkdir -p "${1:-$root/build/wwhd}" && cd "${1:-$root/build/wwhd}" && pwd)

mapfile -t flags < <(python3 "$root/tools/cemu_flags.py" "$cemu/build/compile_commands.json" Espresso/Interpreter/PPCInterpreterFPU.cpp)
cxx=(clang++ "${flags[@]}" -fno-lto -include "$cemu/src/Common/precompiled.h" -I"$root/src/frontend"
     -I"$root/tools/recomp/runtime" -O2)
pids=()
compile() { "${cxx[@]}" "${@:3}" -c "$1" -o "$out/$2" & pids+=($!); }
compile "$root/src/frontend/cemu_boot.cpp" cemu_boot.o
compile "$root/src/frontend/window_system.cpp" window_system.o
compile "$root/src/frontend/window_system.cpp" window_system_null.o -DWWHD_NULL_GPU
compile "$root/src/gpu/null_gpu.cpp" null_gpu.o -I"$root/src/gpu" -I"$cemu/dependencies/Vulkan-Headers/include"
for f in vk renderer; do compile "$root/src/gpu/vk/$f.cpp" "vk_$f.o" -I"$cemu/dependencies/Vulkan-Headers/include"; done
for f in dispatch imports diff; do compile "$root/src/runtime/$f.cpp" "rt_$f.o"; done
for p in "${pids[@]}"; do wait "$p"; done

# libCemuCafe.a without Latte (object names come from compile_commands.json; they are unique)
python3 - "$cemu/build/compile_commands.json" > "$out/latte-objects.txt" <<'PY'
import json, os, sys
for e in json.load(open(sys.argv[1])):
    f = e["file"]
    if "/CemuCafe.dir/" in e["command"] and "/src/Cafe/HW/Latte/" in f and "/LatteAddrLib/" not in f:
        print(os.path.basename(e["command"].split(" -o ")[1].split()[0]))
PY
cp "$cemu/build/src/Cafe/libCemuCafe.a" "$out/libCemuCafe_nolatte.a"
xargs -a "$out/latte-objects.txt" llvm-ar d "$out/libCemuCafe_nolatte.a"

cd "$cemu/build"
base=$(ninja -t commands "$cemu/bin/Cemu_release" | tail -1)
base=${base#": && "}; base=${base%" && :"}
base=${base//" src/gui/wxgui/libCemuWxGui.a"/}
base=${base/"-Xlinker --dependency-file=src/CMakeFiles/CemuBin.dir/link.d"/}
link() {  # link NAME OBJECTS...
    local name=$1 cmd=$base; shift
    cmd=${cmd/"-o $cemu/bin/Cemu_release"/"-o $out/$name $* -Wl,-Map=$out/$name.map"}
    [ "$name" = wwhd-null ] && cmd=${cmd//"src/Cafe/libCemuCafe.a"/"$out/libCemuCafe_nolatte.a"}
    echo "wwhd: linking $name"
    eval "$cmd"
}
link wwhd "$out/cemu_boot.o" "$out/window_system.o"
null_objs=("$out/cemu_boot.o" "$out/window_system_null.o" "$out/null_gpu.o" "$out"/vk_{vk,renderer}.o "$out"/rt_{dispatch,imports,diff}.o)
if [ -n "$recomp" ]; then
    null_objs+=("$recomp"/shard_*.o "$recomp/func_table.o" "$recomp/imports.o")
    echo "wwhd: wwhd-null links the recompiled program from $recomp ($(ls "$recomp"/shard_*.o | wc -l) shards)"
else
    echo "wwhd: wwhd-null without recompiled code (the runtime interprets)"
fi
link wwhd-null "${null_objs[@]}"

# Relink wwhd-null with its archive members as explicit objects in wwhd's inclusion order, so
# static constructors (and with them Cemu's SysAllocator slots in guest memory) run in the same
# order as in wwhd and the reference; otherwise host-side addresses in the 0x0E000000 area differ.
members=$out/members; rm -rf "$members"; mkdir -p "$members"
ordered=()
while IFS=$'\t' read -r archive member; do
    dir=$members/$(basename "$archive" .a)
    [ -d "$dir" ] || { mkdir -p "$dir"; (cd "$dir" && llvm-ar x "$(cd "$cemu/build" && readlink -f "$archive")"); }
    ordered+=("$dir/$member")
done < <(python3 "$root/src/link_order.py" "$out/wwhd.map" "$out/wwhd-null.map")
link wwhd-null "${null_objs[@]}" "${ordered[@]}"
echo "wwhd: built $out/wwhd $out/wwhd-null (${#ordered[@]} members in wwhd order)"
