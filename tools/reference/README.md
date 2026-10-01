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

On the worker worker Cemu renders on the Intel iGPU (Intel iGPU) through Mesa's anv driver. Xvfb has
no DRI3, so `run.sh` sets `MESA_VK_WSI_DEBUG=sw`: the GPU renders and Mesa copies each frame into
Xvfb through the CPU. `REF_GPU=llvmpipe` renders in software instead (Mesa lavapipe): about 1.5x
slower on menus and slower still in 3D, and its frames agree with the GPU's to 49-55 dB.

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
the pinned commit (`c717fcab`), with the patches in `cemu-patches/`, running in the worker
worker (`tools/worker/setup-volume.sh cemu`, then `cemu-rebuild` after patch changes).

**Status (2026-09-28): deterministic.** `determinism.sh` ran two fresh boots to frame 600 and
got **64,874,243 identical trace records**: every OS call, timeslice, alarm and time-jump. Each
run takes about 1–2 minutes on the worker.

What it took. Each numbered item is a patch; the last is a profile setting:

| Patch | What | Why (the divergence it removed) |
|---|---|---|
| 0001 | Virtual clock (`CEMU_VIRTUAL_CLOCK=1`) and the HLE call tracer (`CEMU_HLE_TRACE=file.zst`) | Guest time comes only from executed instructions; idle skips jump to the next alarm, vsync or audio frame. Vsync comes from the CPU scheduler after the GPU retires; audio frames every 3 ms of guest time; the date is pinned. |
| 0002 | Trace exit-at-frame (`CEMU_HLE_TRACE_EXIT_FRAME`); 300 guest cycles per OS call | OS calls cost no guest time, so WWHD's `OSSendMessage`/`OSReceiveMessage` ping-pong (~65k per frame) stalled guest time. |
| 0003 | IPC replies wait host-side | IOSU host threads delivered replies at host-timed moments. |
| 0004 | Synchronous GPU submissions (wait for retire, 2 s cap) | GPU completions woke guest threads at host-timed moments. |
| 0005 | `sched.*` events in the trace (timeslices, alarm firings, idle skips) | Debugging aid: pinpointed the next divergence. |
| 0006 | Legacy IOSU ioctls (act, acp, mcp, boss, nim, fpd) wait host-side | `nn_save.SAVEInit` → act suspended the game thread and an IOSU thread resumed it at a host-timed moment. |
| 0007 | Frame-keyed scripted input (`CEMU_INPUT_SCRIPT`) and frame-indexed screenshots (`CEMU_SHOT_FRAMES`) | Repeatable routes and pictures to check them. |
| 0008 | Pro Controller scripted input through padscore/KPAD; `CEMU_NO_GAMEPAD` reports the GamePad absent | The project is single-screen, Pro Controller mode (design D17). |
| 0009 | `CEMU_SWKBD_AUTO` answers the system keyboard (name entry) | New Game asks for a name through swkbd. |
| 0010 | Screenshots written aside and renamed | With a real GPU the render thread lags the CPU; exit-at-frame cut the last capture in half. |
| 0011 | Execution seam: `g_ppcExecuteHook`, null by default, replaces the interpreter loop in `__OSFiberThreadEntry` and `PPCCore_executeCallbackInternal`; the trace's exit-at-frame runs `at_quick_exit` handlers | Not a determinism fix: it is where the recompiled program's runtime (`src/runtime`, design D1) takes over. With the hook null the reference is unchanged (600 frames, 59,531,239 calls equal to the baseline). |
| 0012 | Texture dumps and a texture-cache log: `CEMU_TEX_DUMP_FRAME=N[,M...]` writes every uncompressed GPU-side texture (all mips and array slices; colour as 8-bit PPM, depth and single-channel float as 16-bit PGM) into `CEMU_TEX_DUMP_DIR` at those swaps (`CEMU_TEX_DUMP_ADDR=hex` limits it to one address); `CEMU_TEX_WATCH=hex` logs creation, deletion, reloads, syncs and clears of the textures at that address | Not a determinism fix: ground truth for the recomp's renderer (G2). `compare_dumps.py` compares a dump with the renderer's (`WWHD_RENDER_DUMP`). Off unless set. |
| 0013 | Texture relations decided by containment, not address order (`LatteTexture_TrackTextureRelation`) | A rendering fix, not a determinism one. A GX2 surface whose mip chain is allocated *below* its base level (WWHD's ambient-occlusion depth: 960x540 R16F at `f4200000`, level 1 drawn at `20009000`) never became related to the texture drawn at the mip address, so the chain's level 1 stayed zero and the reference's ambient occlusion was weaker than the console's. OS-call traces unchanged (59,531,239 and 172,954,163 calls). |
| profile | Single-core *interpreter* (`0005000010143500.ini`) | The single-core recompiler's background JIT made timeslice boundaries depend on host timing. |

`g0_gx2.py TRACE --imports build/recomp/imports.cpp` (design D15, G0) lists every GX2 call on a
route with its argument values, and the imported GX2 functions that are never called.

**Comparing frames and surfaces with the recomp's renderer (G2).** `compare_frames.py REF OURS
[--threshold DB]` compares two directories of `f<N>.tv.ppm` captures. The reference's shot N is the
image its *(N+1)*th swap presents (the screenshot is requested at the Nth `GX2SwapScanBuffers` and
taken at the renderer's next present); the renderer captures the same way. `compare_dumps.py FRAME
REF_DIR OURS_DIR` compares the reference's texture dump (patch 0012) with the renderer's surface
dump at the same swap, in the reference's write order, so the first surface that differs is where
a difference starts. Two things in the reference's texture cache matter for such comparisons:
* It deletes a GPU-written texture that a later write to overlapping memory made stale, and reloads
  it from guest memory (zeros, since the GPU never writes back), when a round-robin scan at a swap
  (25 textures per swap) finds it unused for 100 ms of wall time. Which frames that happens at
  depends on the host; the renderer does it at every swap.
* Its depth clears also clear the color textures that start at the same address, to the depth value.
* Before patch 0013 it never related a mip chain allocated below its base level to the textures
  drawn at the mip addresses, so WWHD's ambient-occlusion depth read zeros for level 1.

`route.sh OUT FRAMES ROUTE BASELINE` is `determinism.sh` with one run: it plays the route once and
compares the trace with a known-good one (for example `wwhd-null` in diff mode against
`/wwhd/data/traces/null-route.zst`), in half the time.

`stream_check.sh save|route NAME` runs `wwhd-null` along a route and checks three things against
baselines from Cemu's own libraries: the trace, the GPU command stream (a hash per frame of every
packet) and the sound (a hash of every mixed block). Our OS layer and forks (`src/os`, design D18)
must leave all three exactly as Cemu's.

`timing.sh save|route OUT` runs `wwhd-null` along a route without the trace (which costs ~40%),
ending at the route's last frame with `WWHD_EXIT_FRAME`, and profiles it (`WWHD_PROFILE`): wall time,
speed against real time, CPU time per thread; `tools/profile_report.py OUT/profile.txt` for where it
went.

`shader_cache_check.sh OUT` and `shader_list_check.sh OUT` render the save route on lavapipe with
the virtual clock, twice each, from an empty shader cache (design D20): the cache's second run, and a
first start from the shader list that a capture run made (`WWHD_SHADER_SOURCES`), must give the same
captures, byte for byte, and meet nothing during play that wasn't prepared before it.

`hle_trace.py diff` normalizes Cemu's `PPCCallback<host pointer>` stub names, which change with
ASLR. `--ignore-core` compares without the core index.

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

* ~~Hardware rendering~~: done (2026-09-29), see above.
* **Routes from a save.** `REF_SAVE=dir` installs a save before boot (see `run.sh`).
  `routes/continue-100.txt` continues quest log 1 of the 100% save (`/wwhd/data/saves/wwhd_100` on
  the worker, not in git) and is in gameplay on the Outset dock at f870. Deterministic: two runs to
  f1800 give identical traces (172,954,163 calls).
* ~~Deeper coverage / input script~~: done. `determinism.sh OUT 10800 routes/title-to-game.txt`
  gives 1,124,796,468 identical records along the route into gameplay (2026-09-29). Screenshots
  of the two runs differ in a few hundred edge pixels (llvmpipe), which the CPU check ignores.
