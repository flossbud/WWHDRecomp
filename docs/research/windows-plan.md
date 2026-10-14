# The Windows port (m-windows)

The roadmap's "platforms" (Linux arm64 and Android are done: `android-plan.md`). This file is the survey of the code
(2026-10-14, main 4329a6a8) and a proposed order of work; nothing is changed yet (main is asking the owner for the
priority). Windows x86-64 only.

The starting point is better than Android's: **Cemu is Windows-first**. Its Windows sources (`util/Fiber/FiberWin.cpp`,
`util/MemMapper/MemMapperWin.cpp` for the 4 GB guest reservation, `Common/ExceptionHandler/ExceptionHandler_win32.cpp`,
`Common/FileStream_win32.cpp`, `wWinMain`, `CemuCommonInit`'s working directory and priority), its CMake (MSVC or
clang-cl, the static CRT, vcpkg's `x64-windows-static`, lld-link and llvm-lib under clang-cl) and its vcpkg manifest
all exist. What's missing is ours: the forks that replaced Cemu's Unix files, the runtime, the frontend and the tools.

## The toolchain

- **clang, not MSVC's cl**: the generated program needs `[[clang::musttail]]` (`tools/recomp/generate.py`,
  `runtime/ppc_ops.h`), an `__asm__` symbol label (`ppc_ops.h`, `runtime/dispatch.cpp`) and `__builtin_*`.
- **The MSVC ABI (clang-cl)**, as Cemu's Windows build. MinGW gets neither branch of Cemu's `if(MSVC) ... elseif(UNIX)`.
- **Flags** (`src/CMakeLists.txt`, `src/build.sh`):
  - `-ffp-contract=off` and `-fsigned-char` need `/clang:` (clang-cl warns and drops the GCC spellings); `-include`
    becomes `/FI`.
  - The ThinLTO cache becomes `/lldltocache:`. Drop `-z noexecstack`, `--strip-debug` and `--version-script`.
  - PGO links clang_rt's profile library by name.
  - The main thread's stack is 1 MB there (8 MB on Linux): `/STACK`.
  - Never `/arch:AVX2`: the emitted `a*c+b` would fuse into FMA.
- **No `/bigobj`**: clang's COFF writer switches to it itself. The 156 shards (~79 MB of objects, compiled twice: the
  replay and the play builds) link with lld-link (link.exe can't read ThinLTO bitcode). Release has no `-g`; a full
  PDB over both shard sets would be big and slow to link.

## Where it builds

1. **Cross, on the worker.** clang 19, lld-link, llvm-lib, llvm-rc and llvm-mt are already in the image.
   - Missing: the MSVC CRT and the Windows SDK (xwin fetches them; Microsoft's licence is the owner's call), and the
     vcpkg dependencies.
   - vcpkg cross-builds CMake ports to the MSVC ABI with a chainloaded clang-cl toolchain, but openssl (Configure +
     nmake), boost's b2 builds and curl's Windows options want a Windows host.
2. **A Windows VM**: VS Build Tools + LLVM (clang-cl) + vcpkg + Ninja, Cemu's own route.
   - The generated sources come from the worker (generate.py's C++ is portable).
   - Costs ~60-80 GB of disk (the worker is at 76% of 246 GB), 8+ GB RAM and a licence.
   - Also the only way to play-test without a real PC (with a GPU passed through).
3. **GitHub's Windows runners** (free for the public repo): never the game, so never the generated program, the routes
   or the checks. Only the vcpkg dependencies (a binary cache or `vcpkg export`) and a compile check of everything
   but the generated program.

**Proposal: 3 for the dependencies plus 1 for the program.**
- CI builds `x64-windows-static` once per baseline; the worker cross-builds the program against it with clang-cl and
  xwin's SDK, and runs the checks under Wine (below). No VM to keep, and the game never leaves our machines.
- 2 instead if the owner has, or wants, a Windows machine; a real Windows PC is needed for W4 either way.

## How it's checked

- **MATCH** with `wwhd-null.exe` headless under Wine on the worker:
  - wine64 goes into `tools/worker/Dockerfile`. A wrapper like `tools/arm64/wrap.sh` (`exec wine wwhd-null.exe "$@"`)
    stands in for `build/wwhd/wwhd-null`; `WWHD_HOME` points it at the scripts' `portable/` (`cemu_boot.cpp`).
  - Unix paths in the environment resolve through `Z:`.
  - The same traces, stream and sound baselines apply once P1 item 14 is done.
  - Captures: winevulkan to the worker's lavapipe, against the same g3 references.
- **Caution:** Wine's C runtime isn't Microsoft's (its math functions are Wine's own), so MATCH under Wine says nothing
  about UCRT's libm. Item 11 removes the host libm from the deterministic paths instead.
- **Real Windows** (the driver, input, timers, non-ASCII paths, the release zip): a VM or a PC, W4.

## P0: doesn't configure, build or link

1. **Fibers.** forks.txt replaces `util/Fiber/FiberUnix.cpp`, which CemuUtil doesn't build on WIN32 (it builds
   `FiberWin.cpp`), so `src/CMakeLists.txt` stops with FATAL_ERROR.
   - Our switch is System V asm, and its guard sends `_WIN32` to `<ucontext.h>`.
   - Fork `FiberWin.cpp` instead: `CreateFiberEx(..., FIBER_FLAG_FLOAT_SWITCH, ...)`, keeping our early return on a
     switch to self (undefined for SwitchToFiber). SwitchToFiber makes no system call, so the reason we replaced
     swapcontext doesn't apply.
   - A hand-written switch would also have to save rdi, rsi and xmm6-15, swap the TEB's stack limits, and carry
     unwind info.
2. **The write watch** (`runtime/write_watch.cpp`): sigaction, siginfo, sigaltstack, mmap, mprotect and sysconf.
   - Windows: VirtualProtect, and a vectored exception handler added first (`AddVectoredExceptionHandler(1, ...)`, ahead
     of Cemu's in ExceptionHandler_win32). The faulting address is in ExceptionInformation[1], a write when [0] is 1.
     Return CONTINUE_EXECUTION, or CONTINUE_SEARCH to chain.
   - No alternate stack (the handler runs on the faulting fiber's). The page size comes from GetSystemInfo.
   - VirtualProtect fails on reserved, uncommitted pages, which MemMapperWin leaves (Linux's mprotect doesn't), so its
     fallback path runs more often.
   - `runtime/tests/write_watch_test.cpp` (fork, wait, rlimit) is Linux-only.
3. **The profiler** (`runtime/profile.cpp`, always built): SIGPROF, setitimer, execinfo, link.h, ucontext's REG_RIP,
   process_vm_readv, `/proc/self`, dl_iterate_phdr, and `%lx`/`unsigned long` casts (32 bits on Windows).
   - Leave it out on Windows, or rewrite it as a sampler thread (SuspendThread, GetThreadContext, RtlVirtualUnwind).
4. **coreinit_Thread.cpp** (our fork): `<pthread.h>`, `clock_gettime`, `clockid_t` and `pthread_getcpuclockid`.
   - Windows: QueryThreadCycleTime (GetThreadTimes ticks every ~15.6 ms).
5. **The frontend's POSIX:**
   - `window_system.cpp`: Xlib and the VRR XChangeProperty, guarded only by `!__ANDROID__` (make it `__linux__`).
   - `cemu_boot.cpp`: `<unistd.h>`, `ssize_t`, readlink of `/proc/self/exe`.
   - `pause_menu.cpp`: dirent, fcntl, unistd, getcwd, and Change Files' restart (`/proc/self/cmdline`, FD_CLOEXEC over
     `/proc/self/fd`, execv of `/proc/self/exe`). Windows: GetCommandLineW, CreateProcessW without inheritance, `_exit`.
   - `null_gpu.cpp`: pthread_exit (ExitThread, as Cemu's LatteThread).
   - `gpu/vk/vk.cpp`: `dlopen("libvulkan.so.1")`. Windows: `LoadLibrary("vulkan-1.dll")` as Cemu's VulkanAPI.cpp, or
     SDL's loader.
   - `PPCTimer.cpp`: `<unistd.h>` only for `_exit`.
6. **setenv**, which UCRT lacks (`os/settings.cpp`, `gpu/vk/lsfg.cpp`, `gpu/vk/renderer.cpp`): a wrapper over
   `_putenv_s` keeping "don't overwrite" (`_putenv_s(k, "")` deletes). Also `M_PI` (`overrides/aspect.cpp`):
   `std::numbers::pi`.
7. **Windows.h's macros**, which reach every file through Cemu's precompiled header (`Common/windows/platform.h`):
   - WinBase.h's `Yield()` breaks `wwhd::rt::Yield` (`rt_internal.h`, `dispatch.cpp`, `imports.cpp`).
   - minwindef.h's `far` breaks a member in `gpu/vk/motion.cpp`.
   - Rename both.
8. **Weak references with no definition** (ELF resolves them to null; COFF weak externals need a default):
   - `recomp_tables.h` (`Linked()`), `PPCTimer.cpp`, `build_kind.cpp` (`__llvm_profile_write_file`), `profile.cpp`,
     `without_forks.cpp`, `window_system.cpp` (`g_rtLooseBuild`).
   - Give each one a defined default.
9. **CMake and vcpkg:**
   - **The CRT:** our object libraries and executables default to `/MD` against Cemu's and vcpkg's `/MT` (LNK2038):
     set `CMAKE_MSVC_RUNTIME_LIBRARY` globally.
   - **SDL3:** from vcpkg (its vulkan feature, as `tools/android/build.sh` does), not `/wwhd/opt/sdl3` (a Linux build).
   - **The vcpkg manifest:** trimmed as for arm64 (wxwidgets, tiff, libusb); `ENABLE_LIBUSB/HIDAPI/BLUEZ/WAYLAND/
     FERAL_GAMEMODE` off (Cemu's libusb lookup hardcodes an `x64-windows` pkgconf path). X11, GTK3, Wayland, bluez
     and gamemode drop out by themselves.
   - **The executable:** the WIN32 subsystem (Cemu's `wWinMain`: UTF-16 arguments; a console app converts `-g`'s
     path through the ANSI code page), plus `cemu.rc` and the manifest (DPI awareness; imgui_extension's
     FontAwesome resource).
   - **Sources:** the Windows `write_watch` and fiber, and `profile.cpp` left out.

## P1: builds, but would differ from x86 Linux (no byte-exact MATCH) or go wrong in play

10. **Thread-locals across fiber switches** (the arm64 class: a guest thread moves between host threads at a yield).
    - Linux x86-64 is safe because every access is `%fs:offset` (`docs/research/threads.md`).
    - Windows reaches TLS through the TEB (`gs:[0x58]`, then `[_tls_index]`), and clang may keep that pointer across a
      call that switches fibers. Clang has no `/GT`, and Cemu's `TLS_WORKAROUND_NOINLINE` is empty under clang-cl.
    - Take the arm64 routes on `_WIN32` too: LockCount noinline (`coreinit_Scheduler.cpp`), `coreinit_Thread.cpp`'s,
      and ppc_ops.h's context table (`WWHD_RT_CTX_TABLE`, which also saves `RT_STORE`'s three dependent loads).
    - Audit what's read after a yield: `g_rtStep`, `g_rtHold`, `g_rtLateNotes`, `g_rtNote`, `g_quiet`,
      `g_rtTickWindow`, `t_assignedCoreIndex`.
    - Teach `tools/sixty/perf/tls_across_calls.py` the `gs:[0x58]` form.
11. **The host's math library on deterministic paths.** UCRT (and Wine's) differ from glibc in the last bit, and UCRT
    picks FMA3 code per CPU (`_set_FMA3_enable(0)`). The calls:
    - `__builtin_pow` in the generated step rules (`ppc_ops.h`: `rt_step_approach`, `rt_step_damp`,
      `rt_step_approach75`) and `std::pow` in `sixty_step.cpp`'s Approach: every 60 fps run, so regress. Their float
      rounding hides most last-bit differences, not all.
    - `cos` in the voices' biquad (`os/snd_core/ax_voice.cpp`): SOUND MATCH.
    - `tan`/`atan` in `overrides/aspect.cpp` (the picture only), and `sin`/`cos` in sixty.cpp's debug spawn.
    - Ship our own on both platforms, gated by an unchanged regress on Linux: correctly-rounded pow/cos, or exact
      cases for the steps used (1/2: sqrt).
12. **FMA:** `-ffp-contract=off` must reach clang-cl (`/clang:-ffp-contract=off`) on the generated program, the
    overrides and Cemu's targets alike.
13. **Floating-point state across fibers:** FiberUnix keeps MXCSR and the x87 control word per fiber. Win32 fibers do so
    only with FIBER_FLAG_FLOAT_SWITCH. Flush to zero per host thread is in place (`coreinit_Thread.cpp` has the
    `_MSC_VER` branches).
14. **Text-mode files:** `fopen(..., "w")` writes CRLF on Windows, so the files the checks compare with `cmp` never
    match the Linux baselines:
    - the GX2 stream (`gpu/null_gpu.cpp`), the sound (`frontend/audio_sdl.cpp`) and `hashes.txt` (`overrides/sixty.cpp`)
    - also `runtime/diff.cpp`'s CSV, `overrides/pacing.cpp`'s log and `coreinit_Thread.cpp`'s logs
    - open them with `"wb"`
15. **The guest's memory layout:** SysAllocator numbers slots with the same key in static-initialisation order, and MSVC's
    layout rules could change a slot's size. Either would move objects off the reference's addresses: diff
    `CEMU_SYSALLOC_LOG` against Linux's.

## P2: works, needs adapting

- **Launching:**
  - play.sh's environment (`WWHD_WINDOW=1`, `WWHD_RENDER=vk`, `WWHD_NATIVE=on`, `WWHD_CEMU_DATA`, `CEMU_NO_GAMEPAD`)
    built in on Windows as on Android, or a `.cmd`. Without it a double-clicked exe runs headless and interpreted.
  - Errors in a message box (`SDL_ShowSimpleMessageBox`): no console.
- **The program's folder:** `read_symlink("/proc/self/exe")` in pause_menu's `ExeDir()` and `lsfg.cpp` returns nothing.
  Use GetModuleFileNameW or `SDL_GetBasePath()`. `File1()` hardcodes `portable/mlc01`: use `ActiveSettings::GetMlcPath()`.
- **Paths:** UTF-8 in `std::string` paths goes through the ANSI code page, and `path::string()` throws on what it can't
  represent.
  - Where: savestate, settings, game_files, ui_draw, pause_menu, shader_cache, `cemu_boot.cpp`'s `portable.string()`.
  - Use `fs::path` from `_utf8ToPath`, or `SDL_IOFromFile`.
  - `rename()` won't replace an existing file (`savestate.cpp`, `gx2/core/GX2.cpp`): `fs::rename`.
  - `ftell`/`fseek` stop at 2 GB (`savestate.cpp`): `_ftelli64`.
  - Replacing `game/wwhd.wua` while it's open (`game_files.cpp`) is a sharing violation (FileStream_win32 opens without
    FILE_SHARE_DELETE).
- **Where its files live:** always `<exe>/portable`. Cemu falls back to `%APPDATA%\Cemu` when there's no `portable/`.
  Either follow it, or require a folder the user can write (not Program Files).
- **Settings applied at run time:** `settings.cpp` setenvs while other threads `getenv`. UCRT frees the old string
  (glibc leaks it), so a reader can use freed memory: keep these values in a locked map.
- **LowPowered()** reads `/sys` (DMI, power_supply, drm). Use GetSystemPowerStatus, the Vulkan device type, and the
  BIOS product name in the registry.
- **Timers:**
  - Sleeps and the 1 ms waits get ~15.6 ms granularity (null_gpu's pacing, coreinit's idle wait, TCL,
    `without_forks.cpp`): `timeBeginPeriod(1)` or high-resolution waitable timers. Real time only.
  - The per-thread CPU clocks (`CLOCK_THREAD_CPUTIME_ID`: null_gpu, pacing) read 0: QueryThreadCycleTime.
- **Presenting:**
  - Vsync off asks only for MAILBOX: fall back to IMMEDIATE.
  - VRR is DWM's (borderless fullscreen with FIFO): the log's wording.
  - lsfg-vk is a Linux layer: hide its rows (`ForThisPlatform` knows only android and desktop).
- **The menu's own file browser:** `$HOME` and `/`. Use USERPROFILE and the drives.
- **Stacks:** Windows frames are bigger (32 bytes of shadow space, xmm6-15 saved). Recheck the 2 MB fiber stacks.
- **Shipping:**
  - A zip: `wwhd-play.exe` (the static triplet: no DLLs but the driver's `vulkan-1.dll`), `cemu/`, `assets/`, default
    `portable/` files and the launcher.
  - The exe is unsigned (SmartScreen).
  - Deploys over Windows' OpenSSH, renaming the running exe aside first.

## Already fine

- **Cemu's Windows code:**
  - MemMapperWin (the 4 GB reservation, at no fixed address), ExceptionHandler_win32, FileStream_win32, and
    `_utf8ToPath`/`_pathToUtf8` (u8string).
  - CemuCommonInit sets the working directory to the exe's; `wWinMain` and the COM init.
- **Ours:**
  - The scheduler lock is atomic wait/notify (WaitOnAddress in MSVC's library; the futex is Android's only).
  - PPCTimer runs on steady_clock (not Cemu's rdtsc branch).
  - Flush to zero has `_MSC_VER` branches; gettid and prctl are behind `BOOST_OS_LINUX`.
  - `write_watch.cpp`'s TLS-model guard.
  - ppc_ops.h's memory access, CAS and barriers. The `wwhd_rt_membase` label: no leading underscore on Windows x64.
  - `_exit` and `quick_exit` in UCRT; `char` signed on both; save-state records sorted by tag; SysAllocator's `\`.
  - The timeslices' LCG; `printf` formats with `%llu`/`%zu` (profile.cpp's `%lx` aside).
- **SDL3:** the win32 Vulkan surface and instance extensions, WASAPI audio, gamepads (XInput, HID), scancodes, the
  native file picker (GetOpenFileNameW; `Chosen` locks), dropped paths in UTF-8, the screensaver block, borderless
  fullscreen and DPI.

## Order

1. **W1, on Linux, each gated by an unchanged x86 regress.** These don't need Windows:
   - 7 (the renames), 8 (weak defaults), 14 (`"wb"`).
   - 11 (our own pow/cos: regress identical, or a deliberate new md5).
   - The setenv wrapper, `fs::path` for the paths, the exe's folder through SDL, Vulkan through SDL's loader.
2. **W2, the build** on the route the owner picks: CMake and vcpkg (9), the Windows fiber, write watch and frontend
   (1-6), until `wwhd-null.exe` and `wwhd-play.exe` link.
3. **W3, the checks under Wine:** MATCH through the wrapper, then 10, 13 and 15 confirmed (the TLS checker, the
   layout log).
4. **W4, on real Windows:** the renderer, input, audio and timers (P2), then the release zip.
