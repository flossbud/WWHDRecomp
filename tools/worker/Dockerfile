# WWHD recomp worker: every heavy job (Ghidra, Cemu builds and runs, compiling generated code,
# traces) runs in this container on the worker, never on the editing machine container.
# Big tools (Ghidra, the Cemu source build) live on the /wwhd volume, not in the image; see
# tools/worker/README.md. Rebuild the image with tools/worker/image.sh.
FROM debian:trixie

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates curl git rsync unzip zip xz-utils zstd file less procps \
      python3 python3-numpy python3-jinja2 \
      build-essential clang lld llvm cmake ninja-build nasm pkg-config autoconf automake autoconf-archive \
      libtool bison flex \
      libssl-dev zlib1g-dev libshaderc-dev libsdl3-dev \
      freeglut3-dev libbluetooth-dev libgcrypt20-dev libglm-dev libgtk-3-dev libpulse-dev \
      libsecret-1-dev libsystemd-dev libusb-1.0-0-dev wayland-protocols libwayland-dev \
      openjdk-21-jdk-headless \
      xvfb openbox x11-utils xdotool imagemagick \
      mesa-vulkan-drivers vulkan-tools libopengl0 libgl1 libegl1 libglx-mesa0 libgtk-3-0t64 \
      pulseaudio pulseaudio-utils \
    && rm -rf /var/lib/apt/lists/*

# uv for the repo's inline-dependency Python tools
RUN curl -LsSf https://astral.sh/uv/install.sh | env UV_INSTALL_DIR=/usr/local/bin UV_NO_MODIFY_PATH=1 sh

# Same uid/gid as owner on the worker so files on /wwhd keep one owner. GPU access comes from
# `--group-add` at run time (host video=44, render=992), not from groups baked in here.
RUN groupadd -g 1000 owner && useradd -m -u 1000 -g 1000 -s /bin/bash owner
USER owner
ENV HOME=/wwhd/home \
    UV_CACHE_DIR=/wwhd/cache/uv \
    GHIDRA_INSTALL_DIR=/wwhd/opt/ghidra_12.0.4_PUBLIC \
    DISPLAY=:99
WORKDIR /wwhd/WWHDRecomp
CMD ["sleep", "infinity"]
