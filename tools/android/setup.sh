#!/usr/bin/env bash
# tools/android/setup.sh (in the worker's container, tools/worker/w): the Android SDK and NDK in /wwhd/opt/android,
# for the app's build (m-android A2, docs/research/android-plan.md): Google's command-line tools, then with their
# sdkmanager the NDK (r27c, LTS), a platform, build-tools and platform-tools (adb). The JDK is the image's.
# Idempotent; ~4 GB. Accepting the SDK's licences is part of installing it (sdkmanager --licenses).
set -euo pipefail
A=${ANDROID_DIR:-/wwhd/opt/android}
TOOLS=11076708                    # commandlinetools 12.0
NDK=27.2.12479018                 # r27c
API=35
mkdir -p "$A"
if [ ! -x "$A/cmdline-tools/latest/bin/sdkmanager" ]; then
    zip=/wwhd/opt/dl/commandlinetools-linux-${TOOLS}_latest.zip
    [ -f "$zip" ] || curl -fsSL -o "$zip.part" "https://dl.google.com/android/repository/commandlinetools-linux-${TOOLS}_latest.zip" \
        && mv "$zip.part" "$zip"
    rm -rf "$A/cmdline-tools" && mkdir -p "$A/cmdline-tools"
    python3 -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' "$zip" "$A/cmdline-tools"
    mv "$A/cmdline-tools/cmdline-tools" "$A/cmdline-tools/latest"
    chmod +x "$A/cmdline-tools/latest/bin/"*
fi
sdk=("$A/cmdline-tools/latest/bin/sdkmanager" "--sdk_root=$A")
yes | "${sdk[@]}" --licenses >/dev/null 2>&1 || true
"${sdk[@]}" --install "ndk;$NDK" "platforms;android-$API" "build-tools;$API.0.0" "platform-tools" | grep -v '^\[' || true
[ -f "$A/ndk/$NDK/build/cmake/android.toolchain.cmake" ] || { echo "android: the NDK didn't install" >&2; exit 1; }
# Xlib's headers (arch-independent: the image's, from Debian's libx11-dev and libxrender-dev) for Cemu's platform.h,
# which includes them on every Linux, Android too (tools/android/cmake/FindX11.cmake); nothing links Xlib
# and xcb's (vulkan.h's platform defines)
mkdir -p "$A/x11/include" && rm -rf "$A/x11/include/X11" "$A/x11/include/xcb"
cp -r /usr/include/X11 "$A/x11/include/X11" && cp -r /usr/include/xcb "$A/x11/include/xcb"
# libadrenotools (BSD-2, Billy Laws; a4-drivers): loads a GPU driver the player imported (Turnip, Qualcomm's own) in
# place of the system's, as other emulators on Android do; src/CMakeLists.txt builds it with the app. A pinned commit
ADRENOTOOLS_COMMIT=8fae8ce254dfc1344527e05301e43f37dea2df80
if [ "$(git -C "$A/libadrenotools" rev-parse HEAD 2>/dev/null)" != "$ADRENOTOOLS_COMMIT" ]; then
    rm -rf "$A/libadrenotools"
    git clone -q https://github.com/bylaws/libadrenotools "$A/libadrenotools"
    git -C "$A/libadrenotools" checkout -q "$ADRENOTOOLS_COMMIT"
    git -C "$A/libadrenotools" submodule update -q --init --recursive
fi
echo "android: SDK $A, NDK $A/ndk/$NDK, platform android-$API"
