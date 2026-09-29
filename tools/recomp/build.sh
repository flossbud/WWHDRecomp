#!/usr/bin/env bash
# build.sh [OUT_DIR] - generate the whole program and compile it (milestone M2), on the worker:
#   tools/worker/job start recomp-build tools/recomp/build.sh
# Generated C++ and objects go to OUT_DIR (default build/recomp); never commit them.
# Env: CEMU_SRC (default /wwhd/opt/cemu-src), JOBS (default 8), OPT (default -O2).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
cemu=${CEMU_SRC:-/wwhd/opt/cemu-src}
out=$(mkdir -p "${1:-$root/build/recomp}" && cd "${1:-$root/build/recomp}" && pwd)
jobs=${JOBS:-8}

python3 "$here/generate.py" "$root/orig/0005000010143500_v0/code/cking.rpx" "$out"

mapfile -t flags < <(python3 "$root/tools/cemu_flags.py" "$cemu/build/compile_commands.json" Espresso/Interpreter/PPCInterpreterFPU.cpp)
# strict FP to match the interpreter (the fuzzer uses the same), no strict aliasing (D11)
cxx=(clang++ "${flags[@]}" -fno-lto ${OPT:--O2} -ffp-contract=off -fno-strict-aliasing
     -I"$here/runtime" -I"$out" -Wno-unused-label -Wno-unused-variable)
echo "recomp: precompiling the runtime header"
"${cxx[@]}" -x c++-header -include "$cemu/src/Common/precompiled.h" "$here/runtime/ppc_ops.h" -o "$out/ppc_ops.h.pch"

echo "recomp: compiling $(ls "$out"/shard_*.cpp | wc -l) shards with $jobs jobs"
start=$(date +%s)
compile() {  # compile SRC: object next to it, errors into SRC.log; prints OK/FAIL line
    local src=$1
    if "${cxx[@]}" -include-pch "$out/ppc_ops.h.pch" -c "$src" -o "${src%.cpp}.o" 2> "${src%.cpp}.log"; then
        echo "ok   $(basename "$src")"
    else
        echo "FAIL $(basename "$src")"
    fi
}
export -f compile; export out
CXX_CMD=$(printf '%q ' "${cxx[@]}")
ls "$out"/shard_*.cpp "$out"/func_table.cpp "$out"/imports.cpp \
    | xargs -P "$jobs" -I{} bash -c "cxx=($CXX_CMD); $(declare -f compile); compile {}" \
    | tee "$out/compile.txt" | awk '{n++} n % 20 == 0 {print "recomp: " n " compiled"}'
fails=$(grep -c '^FAIL' "$out/compile.txt" || true)
echo "recomp: $(grep -c '^ok' "$out/compile.txt") ok, $fails failed, $(( $(date +%s) - start ))s"
[ "$fails" -eq 0 ] || { grep '^FAIL' "$out/compile.txt" | head; exit 1; }
