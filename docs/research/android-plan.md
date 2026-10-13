# The Android port (m-android)

The owner asked for it on 2026-10-12, leaving the approach to main. It's tested in game on the owner's Android
handheld (Snapdragon 8 Gen 2, Adreno 740) over adb on the tailnet. Milestones in plan.json's `m-android`: A1 arm64
byte-exact, A2 the app, A3 on the device, A4 polish. This file is the survey of the code (2026-10-12) and the
order of work; `docs/cloud-handoff.md` item 2 is the older note on arm64 Linux.

## How it's checked

An arm64 **Linux** build runs the checks and the fuzzer: the same fixtures, MATCH byte for byte, as on x86. Only then
the Android build (bionic, a shared library in an APK) and the device. Caution: the glibc build hides the fiber gap
below (bionic has no `ucontext`), so the arm64 fiber switch is written first and used on both.

- **Native, the arm64 worker** (`WWHD_ON=mac`, `tools/worker/mac.sh`): an arm64 Debian VM under rootless podman, the
  same tools image built for arm64; the repo's normal `tools/recomp/build.sh` and `src/build.sh` build there
  (`VCPKG_FORCE_SYSTEM_BINARIES=1`, `SDL3_DIR=` for vcpkg's SDL3, `JOBS=6`) and `checks.sh` runs as on x86, with
  Cemu's `bin/` (resources, sharedFonts, gameProfiles) copied to its `/wwhd/opt/cemu-src/bin`. It's the owner's
  machine: arm64 jobs only.
- **Cross, from the x86 worker** (`tools/arm64/`): `setup.sh` makes a sysroot of Debian's arm64 packages,
  `build.sh` a cross build (`toolchain.cmake`, the `arm64-linux-cross` triplet, a manifest without
  wxwidgets/tiff/libusb/dbus). It builds and links; running it under qemu-user is too slow for the routes (the
  title isn't reached within the checks' 60 s), so the native worker runs the checks.

## A1 result (2026-10-13, native arm64)

- `checks.sh`: both routes' OS-call traces, command streams and sound MATCH x86's references (1,124,796,468 calls on
  the route), diff mode 0 mismatches of 1,118,003, captures 14 of 15 PSNR inf and one 51.37 dB (106 pixels, 0.05%,
  f000900): the command stream is identical, so that is llvmpipe rasterizing differently on arm64 (the device draws
  with its own GPU). Statetests not run there yet.
- Items 2, 11, 12 (`fctiw`, Cemu's quantize: cemu-patch 0017), 15 and 16 done, each byte-identical on x86 (checks,
  regress, statetest).
- The fuzzer (`tools/recomp/fuzz`, `FUZZ_WWHD` to link against our build, `FUZZ_DUMP` for a per-case hash,
  `FUZZ_FTZ` for the runtime's flush to zero) on both hosts: on arm64 our code matches Cemu's interpreter as on x86
  (psq_lux/psq_stux fail on both: Cemu has none). Across hosts (11,589 cases, 200 random states each, the same
  inputs on both), every integer, branch, system and integer load/store case matches; 1,681 floating-point ones
  differ, only as items 13 and 14 said:
  - against x86 with FZ (the runtime's): 1,044 differ in value (denormal inputs: arithmetic, compares, frsqrte,
    psq_l's float loads), 619 in a NaN's sign only, 18 in a NaN's payload only;
  - against x86 with FZ and DAZ (`FUZZ_FTZ=2`, inputs flushed as arm64's FZ does): 21 differ in value, all
    single-precision multiply-adds rounding to the edge of the single denormals (tininess before vs after
    rounding), and the rest in a NaN's sign (1,146) or payload (23).
  So arm64 runs as x86 would with DAZ, but for the default NaN's sign (arm64 makes 0x7FF8..., as the Espresso does;
  x86 0xFFF8...). Neither shows on the checks' routes (the game rarely makes denormals or invalid operations).
  Making them byte-exact would cost a test on every float operation; left as known differences, to look at only if a
  route differs on the device.

## A2 progress (2026-10-13, the app builds)

On the x86 worker, after `tools/android/setup.sh` (the SDK, NDK r27c, platform 35, build-tools, adb in
`/wwhd/opt/android`; never in git): `tools/android/build.sh` builds `build/android/libwwhd-play.so` and
`tools/android/apk.sh` packs it with SDL's Java and `android/` into `build/android/windsong.apk` (Gradle-free: javac,
d8, aapt2, zipalign -P 16, apksigner with a debug key). Both are generated output, never in git.
- vcpkg: the `arm64-android-wwhd` triplet (API 33, static) with the NDK's toolchain chainloaded and a trimmed manifest
  (no wxwidgets/tiff/libusb/dbus) (6).
- Cemu's CMake without a cemu-patch (5): X11/Wayland/bluez/HIDAPI/gamemode/discord/libusb/OpenGL off,
  `tools/android/cmake` (a header-only FindX11: Cemu's platform.h includes Xlib's headers on every Linux, nothing links
  it; the image's X11 and xcb headers copied by setup.sh) and `project.cmake` (those headers and a `log/log.h` stub for
  Cemu's Vulkan headers, everywhere). API 33 for bionic's `backtrace()` (Cemu's exception handler; its ELF symbol
  table built too).
- A shared library (7): `wwhd_executable` makes a SHARED library on Android, 16K pages, exports only `SDL_main`,
  `JNI_OnLoad`, `Java_*` (`src/frontend/android.map`); `android_glue.cpp`'s `SDL_main` calls Cemu's `main()` (it
  keeps its name: `SDL_MAIN_HANDLED`) and stubs `XInitThreads`. 127 MB with symbols, NEEDED only the NDK's system
  libraries.
- TLS (8): `WW_TLS` is plain `thread_local` on Android. The emitter's `stwcx.` uses `__atomic_compare_exchange_n`
  (libc++ has no `std::atomic_ref`; the same instruction on x86).
- Paths (9): the activity (`org.windsong.app.WindSongActivity`, an SDLActivity) sets `WWHD_HOME` (the app's external
  files dir: `cemu/` for Cemu's resources and game profiles, `game/wwhd.wua`, portable/) and the settings
  (`WWHD_NATIVE=on`, `WWHD_RENDER=vk`), and passes `-g game/wwhd.wua`; `cemu_boot`'s and the pause menu's exe dir is
  `WWHD_HOME`. For A3 the files are pushed with adb; the system file picker (content URIs) is S3's.
- X11 (4): window_system.cpp's Xlib and its VRR property are compiled out. The loader name (10) is session bottom's
  (b-android-vk).
- Gated on x86 (the change is a no-op there): checks all MATCH, regress da06a4ee, statetest 0 differ.
Not run on the device yet (A3): it needs the handheld on the tailnet with Wireless debugging paired. Then
`tools/android/device.sh` (in the worker's container): `pair`/`connect`, `install`, `files` (deploy.sh's layout, the game
once), `run` (start, then the app's logcat), `stop`, `uninstall` (restores the device).

## P0: doesn't build, link or start on Android

1. **Fibers** (`src/runtime/fiber/FiberUnix.cpp`): the switch is x86-64 asm; the fallback is `ucontext`, which bionic
   lacks. An aarch64 switch: x19-x28, x29/x30, d8-d15, sp, FPCR.
2. **`__arm64__` is Apple's macro**: Linux and Android define only `__aarch64__`. `coreinit_Thread.cpp` (flush to zero,
   the fiber entry's hi/lo split) and `FiberUnix.cpp` test `__arm64__`, so flush to zero would silently never be set.
3. **profile.cpp** reads `REG_RIP`/`REG_RSP` outside its `__x86_64__` guard (aarch64: `uc_mcontext.pc`/`.sp`).
4. **X11** in `src/frontend/window_system.cpp` (Xlib always included; VRR by XChangeProperty).
5. **Cemu's CMake** treats Android as desktop Linux (X11, Wayland, GTK3, bluez, gamemode): a cemu-patch with
   `NOT ANDROID` and those options off.
6. **vcpkg for arm64-android**: the triplet, the NDK toolchain chainloaded, ANDROID_PLATFORM >= 29 (ELF TLS), a manifest
   overlay without wxwidgets/tiff/libusb; SDL3 built for Android with its Java activity.
7. **A shared library**: `libmain.so` with `SDL_main` in an APK (Gradle), not an executable with `main()`.
8. **Initial-exec TLS** (`write_watch.cpp`) in a dlopen'd library: drop the attribute on Android.
9. **Paths**: `/proc/self/exe` is `app_process64`; portable/ in the app's storage, Cemu's resources and game profiles
   from the APK, the player's dump through the system file picker (content URIs).
10. **The Vulkan loader** is `libvulkan.so` on Android (`src/gpu/vk/vk.cpp`).

## P1: builds, but would differ from x86 (no byte-exact MATCH)

11. **FMA contraction**: aarch64 clang contracts `a*b+c` by default. Only the recompiled program and the overrides have
    `-ffp-contract=off`; Cemu's FPU/paired-single helpers, `src/os` (the audio mixer), gx2 and the runtime don't.
    Make it global, Cemu's targets too.
12. **Float to int out of range**:
    - `fctiw` with a NaN casts it (undefined; x86 gives 0x80000000 as the PPC does, ARM gives 0): `ppc_ops.h`.
    - Cemu's `quantize` for s8/s16 does `(uint32)` on a negative float: x86 wraps, ARM saturates to 0, so every
      signed psq_st of a negative value would store 0. Use `(uint32)(sint32)`.
    - Others (e.g. `ax_mix.cpp`'s sample scaling): find them with `-fsanitize=float-cast-overflow` on x86.
13. **Denormals**: x86 sets MXCSR FZ only (outputs flushed, inputs not); ARM's FPCR.FZ flushes inputs too, and the
    target lacks FEAT_AFP to choose x86's way. lfs/stfs convert through integers (safe); dequantize, fres/frsqrte and
    audio can differ. FZ on every host thread that runs guest code, kept across fiber switches.
14. **The default NaN's sign**: x86 0xFFF8..., ARM 0x7FF8.... Matters wherever an invalid operation's NaN reaches
    memory (the fuzzer forgives NaNs in registers, not in memory).
15. **Memory order**: `sync`/`isync`/`eieio` emit nothing and guest stores are plain; with 3 cores ARM can reorder a
    guest lock's release stores. Emit fences for them (`tools/recomp/emit.py`; on x86 a compiler barrier only).
16. **`char`** is unsigned on Linux/Android aarch64 (signed on macOS arm64): `-fsigned-char`.

## P2: works, needs adapting

- write_watch: ART's signal chain sees each fault first; 16K pages make tracking coarser; link with
  `-z max-page-size=16384`.
- Vulkan: needs 1.3 and dynamic rendering; BC textures (Adreno 740 has them; Mali would need Cemu's decoders);
  logicOp and depthClamp assumed; a 256-512 MB host-visible ring; the surface and swapchain recreated on resume.
- Settings are environment variables `setenv`'d at start (fine in-process; no shell to pass them). `LowPowered()` reads
  `/sys/class/dmi`, `BAT*` and drm, which are wrong or denied on Android.
- Input: SDL's gamepad works; touch for the menu is new.
- Diagnostics: `/proc/self/task`, `process_vm_readv`, `backtrace()` (bionic from API 33): compile out.
- Speed: a -fPIC library reaches the TLS globals (read on every store) through TLSDESC: hidden visibility,
  `-Bsymbolic`, `-mcpu` for the SoC; PGO profiled on the device.

## Already fine

The timer uses steady_clock; `_mm_pause` has Cemu's aarch64 shim; write_watch's pause has an aarch64 branch and reads
only `si_addr`; the 4 GB guest reservation has no fixed address (fits 39-bit spaces); no `-march`/`-msse` anywhere;
Cemu selects its aarch64 paths itself; byte swaps compile to `rev`; the window surface comes from SDL.

## Order

1. Items 2, 11, 12, 15, 16 (small, decide byte-exactness), each gated on x86 (regress unchanged). Done.
2. The fiber switch (1), then 3, 4, 10; an arm64 Linux build and its checks (native, the arm64 worker). Done but 4, 10.
3. 13 and 14 with the fuzzer. Measured (A1 result above).
4. Cemu's CMake and vcpkg (5, 6), then the app (7-9): A2.
5. The device: GPU, input, speed (A3, A4).
