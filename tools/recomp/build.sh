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
# incremental: the header is precompiled again only when it (or the compiler flags) changed, and a
# source compiles only when it is newer than its object or the precompiled header; generate.py
# rewrites only the shards whose text changed
pch=$out/ppc_ops.h.pch
printf '%s\n' "${cxx[@]}" > "$out/flags.new"
if [ ! -f "$pch" ] || [ "$here/runtime/ppc_ops.h" -nt "$pch" ] || ! cmp -s "$out/flags.new" "$out/flags.txt"; then
    echo "recomp: precompiling the runtime header"
    "${cxx[@]}" -x c++-header -include "$cemu/src/Common/precompiled.h" "$here/runtime/ppc_ops.h" -o "$pch"
    mv "$out/flags.new" "$out/flags.txt"
fi

echo "recomp: compiling $(ls "$out"/shard_*.cpp | wc -l) shards with $jobs jobs"
start=$(date +%s)
compile() {  # compile SRC: object next to it, errors into SRC.log; prints OK/FAIL line
    local src=$1 obj=${1%.cpp}.o
    if [ -f "$obj" ] && [ "$obj" -nt "$src" ] && [ "$obj" -nt "$out/ppc_ops.h.pch" ] \
       && [ "$obj" -nt "$here/runtime/recomp_tables.h" ]; then
        echo "ok   $(basename "$src") (up to date)"; return
    fi
    if "${cxx[@]}" -include-pch "$out/ppc_ops.h.pch" -c "$src" -o "${src%.cpp}.o" 2> "${src%.cpp}.log"; then
        echo "ok   $(basename "$src")"
    else
        echo "FAIL $(basename "$src")"
    fi
}
export -f compile; export out here
CXX_CMD=$(printf '%q ' "${cxx[@]}")
ls "$out"/shard_*.cpp "$out"/func_table.cpp "$out"/imports.cpp \
    | xargs -P "$jobs" -I{} bash -c "cxx=($CXX_CMD); $(declare -f compile); compile {}" \
    | tee "$out/compile.txt" | awk '{n++} n % 20 == 0 {print "recomp: " n " compiled"}'
fails=$(grep -c '^FAIL' "$out/compile.txt" || true)
echo "recomp: $(grep -c '^ok' "$out/compile.txt") ok ($(grep -c 'up to date' "$out/compile.txt") up to date), $fails failed, $(( $(date +%s) - start ))s"
[ "$fails" -eq 0 ] || { grep '^FAIL' "$out/compile.txt" | head; exit 1; }
