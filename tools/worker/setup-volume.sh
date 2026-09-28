#!/usr/bin/env bash
# setup-volume.sh [ghidra|cemu|orig|all]  - install the big tools onto the /wwhd volume.
# Runs INSIDE the worker container (tools/worker/w tools/worker/setup-volume.sh all).
#   ghidra: Ghidra 12.0.4 + Maschell/GhidraRPXLoader v0.9.2 -> /wwhd/opt
#   cemu:   Cemu at the pinned commit + tools/reference/cemu-patches, built -> /wwhd/opt/cemu-src
#   orig:   extract code/ and meta/ from the .wua in /wwhd/data/rom -> /wwhd/data/orig
# Idempotent: finished steps are skipped.
set -euo pipefail
repo=/wwhd/WWHDRecomp
opt=/wwhd/opt
CEMU_COMMIT=c717fcab
mkdir -p "$opt/dl"

ghidra() {
    [ -d "$opt/ghidra_12.0.4_PUBLIC/Ghidra/Extensions/GhidraRPXLoader" ] && { echo "ghidra: present"; return; }
    cd "$opt/dl"
    curl -fsSLO https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_12.0.4_build/ghidra_12.0.4_PUBLIC_20260303.zip
    echo "c3b458661d69e26e203d739c0c82d143cc8a4a29d9e571f099c2cf4bda62a120  ghidra_12.0.4_PUBLIC_20260303.zip" | sha256sum -c
    curl -fsSLO https://github.com/Maschell/GhidraRPXLoader/releases/download/v0.9.2/GhidraRPXLoader-0.9.2-d496d43-Ghidra_12.0.zip
    cd "$opt" && unzip -q -o dl/ghidra_12.0.4_PUBLIC_20260303.zip
    unzip -q -o dl/GhidraRPXLoader-0.9.2-d496d43-Ghidra_12.0.zip -d ghidra_12.0.4_PUBLIC/Ghidra/Extensions/
    echo "ghidra: installed"
}

cemu() {
    [ -x "$opt/cemu-src/bin/Cemu_release" ] && { echo "cemu: present"; return; }
    [ -d "$opt/cemu-src" ] || git clone -q --filter=blob:none https://github.com/cemu-project/Cemu "$opt/cemu-src"
    cd "$opt/cemu-src"
    git checkout -q "$CEMU_COMMIT" && git submodule update --init --recursive -q
    git -c user.name=wwhd -c user.email=wwhd@localhost am -q "$repo"/tools/reference/cemu-patches/*.patch
    cmake -S . -B build -DCMAKE_BUILD_TYPE=release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -G Ninja
    cmake --build build -j 10   # matches the container CPU quota (nproc reports 12)
    rm -rf dependencies/vcpkg/buildtrees
    echo "cemu: built"
}

orig() {
    [ -f /wwhd/data/orig/0005000010143500_v0/code/cking.rpx ] && { echo "orig: present"; return; }
    uv run -q "$repo/tools/wua_extract.py" /wwhd/data/rom/*.wua /wwhd/data/orig code/ meta/meta.xml
}

case "${1:-all}" in
    ghidra) ghidra ;; cemu) cemu ;; orig) orig ;;
    all) orig; ghidra; cemu ;;
    *) echo "usage: setup-volume.sh [ghidra|cemu|orig|all]" >&2; exit 2 ;;
esac
