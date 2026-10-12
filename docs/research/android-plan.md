# The Android port (m-android)

The owner asked for it on 2026-10-12, leaving the approach to main. It's tested in game on the owner's Android
handheld (Snapdragon 8 Gen 2, Adreno 740) over adb on the tailnet. Milestones in plan.json's `m-android`: A1 arm64
byte-exact, A2 the app, A3 on the device, A4 polish. This file is the survey of the code (2026-10-12) and the
order of work; `docs/cloud-handoff.md` item 2 is the older note on arm64 Linux.

## How it's checked

An arm64 **Linux** build (glibc, the NDK's clang or the distro's, `--target=aarch64-linux-gnu`) runs the checks and
the fuzzer under qemu-user on the worker: the same fixtures, MATCH byte for byte, as on x86. Only then the Android
build (bionic, a shared library in an APK) and the device. Caution: the glibc build hides the fiber gap below
(bionic has no `ucontext`), so the arm64 fiber switch is written first and used on both.

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

1. Items 2, 11, 12, 15, 16 (small, decide byte-exactness), each gated on x86 (regress unchanged).
2. The fiber switch (1), then 3, 4, 10; an arm64 Linux build and its checks under qemu-user.
3. 13 and 14 with the fuzzer under qemu-user.
4. Cemu's CMake and vcpkg (5, 6), then the app (7-9): A2.
5. The device: GPU, input, speed (A3, A4).
