#!/usr/bin/env bash
# build.sh [OUT_DIR]  - generate the fuzz cases and link the fuzzer against the worker's Cemu build.
# Run on the worker (tools/worker/job start fuzz-build tools/recomp/fuzz/build.sh); then
#   OUT_DIR/fuzz [ITERATIONS=200] [MNEMONIC]
# Env: CEMU_SRC (default /wwhd/opt/cemu-src, built by tools/worker/setup-volume.sh cemu),
#      FUZZ_OPS (a census CSV from tools/recomp/census.py --csv: fuzz only those mnemonics),
#      FUZZ_PER_OP (encodings per mnemonic, default 64).
# The compile flags are Cemu's own for its interpreter (from compile_commands.json) so both sides
# see the same headers and FP settings; the link line is Cemu_release's with -Wl,--wrap=main.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
cemu=${CEMU_SRC:-/wwhd/opt/cemu-src}
out=$(mkdir -p "${1:-$root/build/fuzz}" && cd "${1:-$root/build/fuzz}" && pwd)

ops=()
[ -n "${FUZZ_OPS:-}" ] && ops=(--ops "$FUZZ_OPS")
python3 "$here/gen.py" "$out/cases.cpp" --per-op "${FUZZ_PER_OP:-64}" "${ops[@]}"

# Cemu's flags for PPCInterpreterFPU.cpp, minus its output, PCH and LTO
mapfile -t flags < <(python3 - "$cemu/build/compile_commands.json" <<'EOF'
import json, shlex, sys
for e in json.load(open(sys.argv[1])):
    if e["file"].endswith("Espresso/Interpreter/PPCInterpreterFPU.cpp"):
        a = shlex.split(e["command"])[1:]
        out, skip = [], 0
        for i, x in enumerate(a):
            if skip:
                skip -= 1
                continue
            if x in ("-o", "-c"):
                skip = 1
            elif x == "-Xclang" and i + 1 < len(a) and a[i + 1] in ("-include-pch", "-include"):
                skip = 3
            elif x.startswith("-flto") or x == "-Winvalid-pch":
                pass
            else:
                out.append(x)
        print("\n".join(out))
        break
EOF
)
cxx=(clang++ "${flags[@]}" -fno-lto -ffp-contract=off -include "$cemu/src/Common/precompiled.h"
     -I"$root/tools/recomp/runtime" -I"$here" -Wno-unused-variable)
echo "fuzz: compiling"
"${cxx[@]}" -O2 -c "$here/harness.cpp" -o "$out/harness.o" & p1=$!
"${cxx[@]}" -O2 -c "$out/cases.cpp" -o "$out/cases.o" & p2=$!
wait $p1 && wait $p2

echo "fuzz: linking (thin LTO over Cemu, a few minutes)"
cd "$cemu/build"
link=$(ninja -t commands "$cemu/bin/Cemu_release" | tail -1)
link=${link#": && "}; link=${link%" && :"}
link=${link/"-o $cemu/bin/Cemu_release"/"-o $out/fuzz $out/harness.o $out/cases.o -Wl,--wrap=main"}
link=${link/"-Xlinker --dependency-file=src/CMakeFiles/CemuBin.dir/link.d"/}
eval "$link"
echo "fuzz: built $out/fuzz"
