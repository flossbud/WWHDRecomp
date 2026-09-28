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
* **Boot-time game patch:** the log shows `Patching TWW race conditon at: 0x027f9994`
  (`GamePatch.cpp`). Our recompiler must reproduce it (design D10).

## Still to do for M0a

* **Deterministic clock:** a source build of Cemu with a fixed-step `OSGetTime`, needed for
  frame-exact comparison (D16). v2.6 is an AppImage; the source build will pin the same commit we
  link against.
* **Shared fonts:** the log says "no shareddata fonts loaded", so placeholder text is used. Wire up
  the `CafeStd.ttf` family.
* **Compact GX2 tracer:** a binary call log instead of text logging (see design D15).
* **Input script:** a scripted route (frame-indexed input) instead of ad-hoc `press.sh` calls.
