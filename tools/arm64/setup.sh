#!/usr/bin/env bash
# tools/arm64/setup.sh (in the worker's container, tools/worker/w): an arm64 Linux sysroot and qemu-user in
# /wwhd/opt/arm64, for the arm64 build and its checks under qemu (m-android A1, docs/research/android-plan.md).
# Debian's cross packages, fetched with apt as the container's user (no root: its own lists and cache) and
# unpacked; links make the cross layout (usr/aarch64-linux-gnu) a plain sysroot, so clang needs only
# --target=aarch64-linux-gnu --sysroot=/wwhd/opt/arm64/root. Idempotent; ~540 MB.
set -euo pipefail
A=${ARM64_DIR:-/wwhd/opt/arm64}
R=$A/root
pkgs=(qemu-user libc6-arm64-cross libc6-dev-arm64-cross linux-libc-dev-arm64-cross libstdc++-14-dev-arm64-cross
      libstdc++6-arm64-cross libgcc-14-dev-arm64-cross libgcc-s1-arm64-cross libatomic1-arm64-cross
      # Xlib, which Cemu's platform.h and our window code include on Linux (not on Android): Debian's arm64 packages
      libx11-6:arm64 libx11-dev:arm64 libxrender1:arm64 libxrender-dev:arm64 libxcb1:arm64 libxcb1-dev:arm64
      libxau6:arm64 libxau-dev:arm64 libxdmcp6:arm64 libxdmcp-dev:arm64 libbsd0:arm64 libmd0:arm64 x11proto-dev xtrans-dev
      # EGL, GL and GLX headers, which Cemu's shader decompiler includes (nothing links them)
      libegl-dev:arm64 libgl-dev:arm64 libglx-dev:arm64)
mkdir -p "$A/apt/lists/partial" "$A/apt/cache/archives/partial" "$A/debs" "$R"
O=(-o "Dir::State::Lists=$A/apt/lists" -o "Dir::Cache=$A/apt/cache" -o "Dir::Cache::archives=$A/apt/cache/archives" -o Debug::NoLocking=1
   -o "APT::Sandbox::User=$(id -un)" -o Dir::State::status=/var/lib/dpkg/status
   -o APT::Architectures::=amd64 -o APT::Architectures::=arm64)
apt-get "${O[@]}" update >/dev/null
(cd "$A/debs" && apt-get "${O[@]}" download "${pkgs[@]}" >/dev/null)
# the plain (multiarch) layout over the cross one, made first so the arm64 packages' files land in it
mkdir -p "$R/usr/aarch64-linux-gnu/include" "$R/usr/aarch64-linux-gnu/lib" "$R/usr/lib" "$R/lib"
ln -sfn aarch64-linux-gnu/include "$R/usr/include"
ln -sfn ../aarch64-linux-gnu/lib "$R/usr/lib/aarch64-linux-gnu"
ln -sfn ../usr/aarch64-linux-gnu/lib "$R/lib/aarch64-linux-gnu"
for d in "$A"/debs/*.deb; do dpkg-deb --fsys-tarfile "$d" | tar -x -C "$R" --keep-directory-symlink; done
ln -sfn ../usr/aarch64-linux-gnu/lib/ld-linux-aarch64.so.1 "$R/lib/ld-linux-aarch64.so.1"
echo "arm64 sysroot: $R; qemu: $R/usr/bin/qemu-aarch64 -L $R"
