#!/usr/bin/env bash
# build.sh [OUT_DIR] - generate the whole program (milestone M2), on the worker:
#   tools/worker/job start recomp-build tools/recomp/build.sh
# Generated C++ goes to OUT_DIR (default build/recomp); never commit it. src/build.sh (CMake) compiles
# it into wwhd-null, with Cemu's flags plus -ffp-contract=off -fno-strict-aliasing against a
# precompiled runtime/ppc_ops.h; generate.py rewrites only the shards whose text changed, so only
# those compile again.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out=$(mkdir -p "${1:-$root/build/recomp}" && cd "${1:-$root/build/recomp}" && pwd)

python3 "$here/generate.py" "$root/orig/0005000010143500_v0/code/cking.rpx" "$out"
echo "recomp: generated $(ls "$out"/shard_*.cpp | wc -l) shards in $out; src/build.sh compiles them"
