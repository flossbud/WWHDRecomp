# Reference Cemu

An **unmodified upstream Cemu** run headless on this server, as the oracle for our recompiled
build (see `docs/recompiler-design.md`, M0a and D16). It produces reference frames (TV and
GamePad) and GX2 call logs for scripted input. It is never part of the product.

## Setup (once)

```sh
sudo apt install xvfb openbox mesa-vulkan-drivers vulkan-tools xdotool imagemagick x11-utils \
                 pulseaudio pulseaudio-utils libopengl0 libgl1 libegl1 libgtk-3-0t64 libfuse2t64
mkdir -p ~/opt/cemu && cd ~/opt/cemu
gh release download v2.6 -R cemu-project/Cemu --pattern '*.AppImage'
chmod +x Cemu-2.6-x86_64.AppImage && ./Cemu-2.6-x86_64.AppImage --appimage-extract
mkdir -p squashfs-root/usr/bin/portable     # portable mode: all Cemu state stays in there
```

## Use

```sh
WWHD_GAME=/path/to/game.wua tools/reference/run.sh     # Xvfb :99 + openbox + null audio sink + Cemu
tools/reference/press.sh x                             # press A (mapping in controller0.xml)
tools/reference/shot.sh out/title                      # out/title.tv.png (1280x720), out/title.pad.png (854x480)
REF_LOGFLAG=2 WWHD_GAME=... tools/reference/run.sh      # log every GX2 call to portable/log.txt
                                                       # (~1 GB per 3.5 min: short runs only)
```

Rendering is Mesa lavapipe (Vulkan on the CPU): about 2–5 FPS on this 4-core VM, and the first
boot takes minutes while shaders compile. The title screen is reached about 3 minutes after launch.

## Gotchas found getting here

* **Audio:** without a working audio backend WWHD stalls on its loading screen. A PulseAudio
  null sink plus `<Audio><api>3</api>` (Cubeb) fixes it.
* **Input:** keys are ignored unless Cemu's window is *active*, so a window manager (openbox) is
  required. Key codes in the profile are X keysyms.
* **Startup hang:** Cemu 2.6 sometimes deadlocks in a forked child before logging starts. `run.sh`
  detects this (no "Run title" within 60 s) and retries.
* **Clean frames:** the settings template turns off the FPS overlay and notifications.
  `shot.sh` captures Cemu's render child windows directly.
* **Shared fonts:** the AppImage's portable mode can't find `resources/sharedFonts`, so placeholder
  text is used. The source build finds them in `bin/resources`.
* **Boot-time game patch:** the log shows `Patching TWW race conditon at: 0x027f9994`
  (`GamePatch.cpp`). Our recompiler must reproduce it (design D10).

## Patched source build (deterministic reference)

The AppImage can't provide repeatable runs, so the real reference is Cemu built from source at
the commit the design pins (`c717fcab`), with the patches in `cemu-patches/`:

* **Virtual clock** (`CEMU_VIRTUAL_CLOCK=1`, set by `REF_VIRTUAL_CLOCK=1`):
  * Guest time advances only with executed guest instructions.
  * When no guest thread can run, it jumps to the next alarm, vsync or audio frame.
  * Vsync (every 1/60 s of guest time) is raised by the CPU scheduler once the GPU has retired
    all submitted work.
  * Audio frames are paced every 3 ms of guest time.
  * The calendar date is pinned to 2026-01-01.
  * Combined with single-core CPU mode (`0005000010143500.ini`) and synchronous shader
    compilation (`settings.xml`), two runs with the same input should produce the same guest
    behaviour. `hle_trace.py diff` checks exactly that.
* **HLE call tracer** (`CEMU_HLE_TRACE=file.zst`, optional `CEMU_HLE_TRACE_FILTER=gx2.`): a
  compact binary record of every OS-library call (call index, LR, r3–r10, f1–f8, frame number),
  zstd-compressed. Read it with `hle_trace.py`.

```sh
# dependencies as for the AppImage, plus the build toolchain:
sudo apt install clang lld cmake ninja-build nasm pkg-config autoconf automake autoconf-archive \
    libtool bison flex python3-jinja2 freeglut3-dev libbluetooth-dev libgcrypt20-dev libglm-dev \
    libgtk-3-dev libpulse-dev libsecret-1-dev libsystemd-dev libusb-1.0-0-dev wayland-protocols libwayland-dev
git clone --filter=blob:none https://github.com/cemu-project/Cemu ~/opt/cemu-src && cd ~/opt/cemu-src
git checkout c717fcab && git submodule update --init --recursive
git am /path/to/WWHDRecomp/tools/reference/cemu-patches/*.patch
cmake -S . -B build -DCMAKE_BUILD_TYPE=release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -G Ninja
cmake --build build        # binary: bin/Cemu_release (bin/ already holds resources/ and gameProfiles/)
rm -rf dependencies/vcpkg/buildtrees   # ~7.6 GB of vcpkg intermediates, not needed after configure

CEMU_BIN=~/opt/cemu-src/bin/Cemu_release REF_VIRTUAL_CLOCK=1 CEMU_HLE_TRACE=/tmp/run1.zst \
    WWHD_GAME=/path/to/game.wua tools/reference/run.sh
```

Configure (vcpkg building every dependency) took 13.5 minutes here, and the compile takes
another 30+ minutes. Disk: plan for about 10 GB during the build, or about 2.5 GB after
deleting the vcpkg build trees.

## Still to do for M0a

* **Verify determinism:** two virtual-clock runs with the same input must give identical traces
  (`hle_trace.py diff`). Anything that still differs gets tracked down; GPU-written timestamps
  are a likely suspect.
* **Input script:** a scripted route (frame-indexed input) instead of ad-hoc `press.sh` calls.
