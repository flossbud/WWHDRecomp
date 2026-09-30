#!/usr/bin/env bash
# setup-volume.sh [ghidra|cemu|cemu-rebuild|sdl3|orig|all]  - install the big tools onto the /wwhd volume.
# Runs INSIDE the worker container (tools/worker/w tools/worker/setup-volume.sh all).
#   ghidra: Ghidra 12.0.4 + Maschell/GhidraRPXLoader v0.9.2 -> /wwhd/opt
#   cemu:   Cemu at the pinned commit + tools/reference/cemu-patches, built -> /wwhd/opt/cemu-src
#   cemu-rebuild: re-apply the current patches on the pinned commit and rebuild incrementally
#           (after tools/reference/cemu-patches changed)
#   sdl3:   SDL 3.4.10, the version Cemu's vcpkg build compiles against, static and with video
#           (X11, Wayland), audio and Vulkan -> /wwhd/opt/sdl3. wwhd-null links it in place of
#           vcpkg's, which Cemu builds for controllers only (src/build.sh)
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
    rm -f build/CMakeCache.txt   # a failed earlier configure can cache NOTFOUND tools; vcpkg's installed tree is kept
    cmake -S . -B build -DCMAKE_BUILD_TYPE=release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -G Ninja
    cmake --build build -j 10   # matches the container CPU quota (nproc reports 12)
    rm -rf dependencies/vcpkg/buildtrees
    echo "cemu: built"
}

cemu_rebuild() {
    cd "$opt/cemu-src"
    git checkout -q --detach "$CEMU_COMMIT"
    git -c user.name=wwhd -c user.email=wwhd@localhost am -q "$repo"/tools/reference/cemu-patches/*.patch
    cmake --build build -j 10
    echo "cemu: rebuilt with $(ls "$repo"/tools/reference/cemu-patches/*.patch | wc -l) patches"
}

sdl3() {
    [ -f "$opt/sdl3/lib/libSDL3.a" ] && { echo "sdl3: present"; return; }
    local tgz=libsdl-org-SDL-release-3.4.10.tar.gz cached=$opt/cemu-src/dependencies/vcpkg/downloads
    cd "$opt/dl"
    [ -f "$tgz" ] || cp "$cached/$tgz" . 2>/dev/null || curl -fsSL -o "$tgz" https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.4.10.tar.gz
    echo "0dc11d980ba17250200718fa4e28011da293f27ed92f92203afffe396811f307  $tgz" | sha256sum -c
    rm -rf SDL-release-3.4.10 && tar xzf "$tgz"
    cmake -S SDL-release-3.4.10 -B SDL-release-3.4.10/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DSDL_STATIC=ON -DSDL_SHARED=OFF \
        -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_X11=ON -DSDL_WAYLAND=ON -DSDL_VULKAN=ON \
        -DSDL_PIPEWIRE=ON -DSDL_PULSEAUDIO=ON -DSDL_ALSA=ON -DCMAKE_INSTALL_PREFIX="$opt/sdl3"
    cmake --build SDL-release-3.4.10/build -j 10 && cmake --install SDL-release-3.4.10/build
    rm -rf SDL-release-3.4.10
    echo "sdl3: installed"
}

orig() {
    [ -f /wwhd/data/orig/0005000010143500_v0/code/cking.rpx ] && { echo "orig: present"; return; }
    uv run -q "$repo/tools/wua_extract.py" /wwhd/data/rom/*.wua /wwhd/data/orig code/ meta/meta.xml
}

case "${1:-all}" in
    ghidra) ghidra ;; cemu) cemu ;; cemu-rebuild) cemu_rebuild ;; sdl3) sdl3 ;; orig) orig ;;
    all) orig; ghidra; cemu; sdl3 ;;
    *) echo "usage: setup-volume.sh [ghidra|cemu|cemu-rebuild|sdl3|orig|all]" >&2; exit 2 ;;
esac
