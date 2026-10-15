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
  `WWHD_HOME`. For A3 the files were pushed with adb; since S3 they come from the APK and Setup (below).
- X11 (4): window_system.cpp's Xlib and its VRR property are compiled out. The loader name (10) is session bottom's
  (b-android-vk).
- Gated on x86 (the change is a no-op there): checks all MATCH, regress da06a4ee, statetest 0 differ.
Not run on the device yet (A3): it needs the handheld on the tailnet with Wireless debugging paired. Then
`tools/android/device.sh` (in the worker's container): `pair`/`connect`, `install`, `files` (the game, once; the rest
since S3 from the APK), `run` (start, then the app's logcat), `stop`, `uninstall` (restores the device).

## A3 on the device (2026-10-13, the owner's handheld: Adreno 740, Android 13)
- Runs: three host threads at 59-60 fps in the title (the scheduler thread 72-80% busy); fullscreen and landscape;
  Home and back keep the process; asleep or in another app the pause menu opens and the sound's stream pauses.
- Sound on AAudio directly (audio_aaudio.cpp): no fast track there (no MMAP; a normal mixer track), latency 122 -> 75 ms.
- Found by testing: libc++'s atomic wait/notify lose wake-ups (the scheduler lock on a futex), arm64's thread pointer
  read once a function (the lock count through a call), SDL's orientation and system bars, the start screen's input.
- Open: the title's boat snaps in at 60 (its 60 rules; session bottom). (SDL's event wait spinning: fixed, A4.)
- **t-startscreen S3** (2026-10-14, on the Odin with a scratch copy): the app's own files ship in the APK (apk.sh's
  asset trees) and the activity extracts them when their hash changes: `app/` into the files dir's app/ (Cemu's
  resources and game profiles, the game's own as its default, the shader list, the pause menu's assets;
  `WWHD_CEMU_DATA` and `WWHD_UI_ASSETS` point there), `once/` (settings.xml, the controller profile) only where missing.
  app/ is the app's own folder because folders a test pushed with adb (device.sh's old `files`: cemu/, assets/) are
  the shell user's, which the app can't write in. With no game/wwhd.wua the activity passes no -g and Setup asks:
  Browse opens the system's picker (the menu's own browser never: an app can't list the shared storage), the
  content:// URI is copied to game/wwhd-new.wua with the progress bar ("Copying files"; a .wua's footer and the free
  space checked first), checked there, renamed over game/wwhd.wua once verified, registered. Change Files restarts the
  activity (Restarter, a second process, ends this one and starts it again: no exec on Android). Display, Resolution
  and Variable Refresh are hidden there (`"platform": { "not": "android" }`). `device.sh files` only pushes the game
  for tests. On the device: the picker's choice copied in about 1.5 s, verified, byte-identical, and the game
  booted from it; Change Files back on the start screen within 1 s (a new process), verified; the first start's
  extraction 343 files in 0.5 s, none on the next. Where an older test pushed portable/'s game profile with adb, that
  copy still wins over the APK's (the same content on the Odin).
- The start screen's touch layout (s3-touch, 2026-10-14; the design's README 3a/3b): 1920x864 fitted, the logo's column
  left, the board right, no Quit, Back as the design's table (help -> Got It, checking -> Cancel, Game Files -> the
  menu, the first view -> the app to the background), the touch model (the first button lit, a press scales and its
  release chooses) and a pad's (the desktop's cursor and prompts: the Odin's pad is attached at the start, so it begins
  there; a touch switches). On the Odin, in a test home: Launch over the live title, Game Files by a tap, Back to the
  menu and to the background; Setup (`WWHD_NO_GAME=1`) by touch, its picker opened and closed with Back.
- GPU drivers (a4-drivers): Settings' GPU Driver and Import GPU Driver (Android only): a driver package (zip: meta.json
  and its library, as other Android emulators take) unpacked into the app's internal files and loaded by libadrenotools
  (setup.sh pins it; its hooks in the APK, extracted at install) in place of the system's; the system's when it fails.
  Mr. Purple's Turnip T30 on the Odin: the title's heaviest pan 43.5 fps (frames 28 ms apart, GPU 13 ms) against the
  system driver's 34 (36 ms, GPU 16 ms).
- Tools: `tools/android/device.sh` (ENV="A=1;B=2" for run's variables), the APK debuggable (run-as: simpleperf,
  lldb-server on an abstract socket, the app has no INTERNET permission), Cemu's log in the app's external files
  (portable/log.txt), `adb shell screenrecord` for frame-by-frame looks.

## A4 performance (2026-10-13, session top: a4-perf)
The device's scheduler thread (the game's core 1) was 72-80% busy at 60 in the title on three host threads. Off the
device first, with the same real-time run on both machines (wwhd-play, `WWHD_CORES=3`, no renderer, soak.sh's play from
the 100% save, 2 minutes at 60, the profiler at 64-deep stacks: a scratch script in the shape of soak.sh's):
- **The arm64 worker** (the Mac VM, native): 60.1 fps, the work a frame 7.55 ms (45% of a core); core 1's samples
  84% guest code, the scheduler 6.9%, the runtime 2.4%, HLE 1.3%, fiber switches 1.3%. The biggest host function:
  pthread_cond_broadcast, 5.2% of core 1 (3.6% of all cores), all from `__OSAddReadyThreadToRunQueue` (Cemu's
  CounterSemaphore: a mutex and notify_all on each run queue's 0 -> 1).
- **x86** (the desktop worker): the work a frame 4.65 ms (28%), guest code 87%, the scheduler 3.8%,
  pthread_cond_broadcast 1.5% of core 1.
- Tried on arm64, A/B interleaved, 2 rounds each (the work a frame's median): PGO (pgo.sh run on the arm64 worker,
  the profile matching) 7.55 -> 7.48 ms (~1%, against x86's 11%); `-march=armv8.2-a` (LSE atomics inline, not the
  outline helpers) 7.55 -> 7.53 (none); the run queues' counts as an atomic with the wake only while a core sleeps
  (no mutex or broadcast otherwise) 7.52 -> 7.45, pthread_cond_broadcast 3.4% -> 3.2% of all cores: the broadcasts are
  real wakes of the sleeping cores 0 and 2, not idle ones. None kept but PGO's tools.
- So the device's gap (72-80% against 45% on the Mac's cores) is the device's own: profile it there. Tools:
  `tools/android/pgo.sh [PROFILE [OUT]]` (the app's library with pgo.sh's profile: the workers' LLVM 19 writes it, the
  NDK's clang 18 can't read that indexed format, so it goes through llvm-profdata's text format), then `apk.sh
  OUT/libwwhd-play.so OUT/windsong-pgo.apk`; `tools/android/device.sh perf [SECONDS] [LIB]` (simpleperf on the
  device, --app, frame-pointer call graphs, reported per thread and for the scheduler thread's functions against the
  unstripped library). On the device: the plain and the PGO APKs, perf on each in the title and in play, then the
  scheduler thread's top functions against the Mac's (above): what the device spends that the Mac doesn't.
- **On the device** (2026-10-13, the title at 60, `device.sh perf`): the scheduler thread 73-78% busy at 59.3-59.6 fps
  (plain), 69% with PGO in the one steady sample (not conclusive). By thread: the window's thread ("wwhd") 41% of the
  app's CPU, the game's core 25%, the null GPU's thread 21%, cores 0 and 2 5% each.
  - **The window's thread**: SDL 3.4's Android event wait pumps on every pass, each pump takes the pending poll
    sentinel out and queues a new one, and every event queued sends Android's lifecycle WAKE: its 50 ms waits never
    slept (WWHD_DEBUG_EVENTS: 200 waits in 10 s, all empty, no event queued; simpleperf: SDL_UpdateJoysticks' device
    checks and the clock). The poll sentinel off on Android: 41% -> 0.06%, the pause menu's keys at once.
  - **The game's core**: [linker]tlsdesc_resolver_dynamic 4.8% and the kernel 7.5%. The app's library is dlopen'ed, so
    its thread_locals are dynamic TLS: every guest store's g_rtStoreTable test was a call into bionic's resolver (three
    in f_028E8F64 alone). Android's generated code reads it through ctx->rspTemp, the address of the host thread's
    g_rtStoreTable set as a thread is loaded on a core (__OSLoadThread); the hot function's three calls -> none.
    The address comes from a noinline call (with LockCount's empty asm): the first build had __OSLoadThread inlined
    into __OSThreadSwitchToNext, which reads TPIDR_EL0 once at its top, so after the fiber switch it added the new
    host thread's resolver offset to the old host thread's pointer, and a thread moved between host threads got a wild
    table address (SIGSEGV in the first 2 s on the device). The Mac VM can't show this class: an executable's TLS is
    the thread pointer plus a constant there, so a stale pointer reads another thread's (valid) variable.
    The quantized psq stores (psq_write) take the same path. On the device (fix and rq APKs alternately, the title,
    simpleperf 30 s each): the resolver 4.8% -> 1.7% of the game's core (what's left is mostly rt_journal_store's and
    GX2's own thread_locals), its cycles a frame 30.4 M -> 29.8-30.0 M (1.5-2%); fps and busy share unchanged (59.2,
    76-78%): with less work the governor lowers the core's clock, so on the device cycles a frame is the measure.
  - **The kernel**: 9.9-10.8% of the game's core, 7-8% of it the futex wake in condition_variable::notify_all from
    __OSAddReadyThreadToRunQueue (CounterSemaphore::increment), 1.4% the scheduler lock's own futex under
    OSSendMessage. Bionic's pthread_cond_broadcast makes the syscall with no waiter too (glibc doesn't), but a count
    that signals only while a core waits (tried, not kept) cut it by under 1 point: the rest are real wakes of cores 0
    and 2, asleep in waitUntilNonZero, for every thread readied that may run on them. Fewer of them needs a scheduling
    choice (wake one idle core, or none for a thread the readying core will run next), which changes which core runs
    what: not done.
  - **The first run after the device has dozed a while** ran at ~51 fps (both builds: the window's first run each
    time), the cycles a frame higher too; the runs after it at 59.2. Measure after a warm-up run. A candidate, not
    tried: if it's the governor ramping slowly after a deep doze (or the device's performance mode), a sustained
    performance hint from the app (Activity.setSustainedPerformanceMode, or Android's game mode API) may hold the
    clocks steady.
    Tried 2026-10-14 (session main): Android's performance hints (ADPF: a session over the guest cores' host threads and
    the GPU thread, each frame's critical work reported against 16.67 ms; branch adpf-parked, not merged). After a
    6-minute doze, the title with no start screen, 10 s periods: hints on 57.0, 58.3, 59.4, 59.9, 60.1 fps; off 57.2,
    58.5, 59.5, 59.6, 60.2: the same, and the ~51 fps state didn't come either time. Not kept; try again if the slow
    state is reproduced on purpose.
  - **Two more tried in play (2026-10-14, session main; route sail, the system driver, 20 s simpleperf stat in the
    60 part, A/B/A/B), neither kept:** (1) the write watch sparing the renderer's per-program compare (ProgramHash's
    memcmp): the GPU thread 30.2 -> 29.6-30.9 M cycles a frame, within noise, sail's fps the same (branch phw-parked);
    (2) WWHD_PIN=1, core 1's host thread on the X3 and the others on the A715s: 57.8-58.1 -> 58.3-58.5 fps, core 1's
    thread 2.67 -> 2.70 GHz, but its pin doesn't hold (its Cpus_allowed back to 0-7 within seconds, something on the
    device resets it; the 3-6 pins hold) (branch pin-parked). Sail's game thread runs ~2.7 GHz of the X3's 3.19.

## A4 in play (2026-10-14, session top: a4-play)
Three routes played at 60 on the device by `tools/android/route.sh` (the 836f8997 APK, three host threads): tour3,
sail and wfw, each with the system driver and with Turnip (purpleT30 from a test folder, `ENV=` in route.sh, removed
after each run), simpleperf over the whole run (2000 Hz, frame-pointer call graphs, CLOCK_MONOTONIC) and the samples
split by the route's 10 s real-time windows. (The log's wall times map to the monotonic clock through a log line seen in
both `logcat -v monotonic` and `-v time`: the offset was the same to 1 ms over the four runs that had both, so the device
didn't suspend between runs.) The routes share their first ~1000 frames: 30 to f900 (WWHD_60FPS_FROM), then 60.
- **At 30**: 26.8, 30.1, 28.6 fps: the third window has the first sights (51 shaders 132 ms, 57 pipelines 26 ms), its
  99th 125-143 ms. No first sights after it.
- **The first window at 60** is 54-55 fps on every route with both drivers, with one ~103 ms frame (worst 102-106 ms in
  all six): probably the 30 -> 60 switch at f900, a route's artifact (not checked against the switch's frame). The
  scheduler thread is 77-83% busy in it.
- **After it**: tour3 59.5-60.1 (both drivers), wfw ends there. Sail stays at 57.4-58.1 with the system driver, the
  scheduler thread at 83% throughout; Turnip 59.3-59.5 at the same 83%.
- **The scheduler thread** (OSSched[core=1], at 60): guest code 76-78% of its cycles, a flat profile (the top f_
  function ~1.5%; f_02759564, 4-16%, is where it idles), the runtime 12-13% (psq_store 1.2%, rt_journal_store 1.1%,
  the TLS resolver 1.5%), the kernel 8-10% (one address 6-7%). Not shader compiles, and not the driver.
- **The GPU thread** (NullGPU): Turnip takes ~33% fewer of its cycles (9.4-10k samples a 10 s window against
  14.1-14.8k; the driver's share 19-20% against 31-33%), so the game's thread waits less on it: sail's 1.5 fps. Our
  own part of it, both drivers: memcpy 5-11%, the renderer's Descriptors() 4-7%, memcmp 4-7%, ProgramHash() 4-6% (a
  shader program hashed each draw), atomics ~4%: about a quarter to a third of the thread (session main took it,
  shared with the PC and the Deck).
- **ADPF on sail** (session main, A/B/A/B, simpleperf stat): core 1 at 2.68 GHz either way, 57.5-58.7 fps both: no
  effect (branch adpf-parked). The ~2.7 GHz ceiling (the X3's top is 3.19) is likely the device's performance mode:
  the owner's per-app performance and fan modes (OdinTools) may set WindSong higher.

## a4-cpu: the game thread's host cost (2026-10-14, session top)
Sail at 60 (route.sh's test home, main 3e469320), simpleperf at 4000 Hz with fp call graphs over 40 s, each kernel
sample read back to the user frame that entered the kernel and our frames above it (a scratch script over
simpleperf_report_lib):
- **The game thread (OSSched[core=1])**: guest code 79%, our runtime 9.4%, the kernel 8.0%, the linker's TLS resolver
  1.4%, libc 1.3%. Its kernel time is futex calls:
  - 6.1% from `__OSAddReadyThreadToRunQueue` (5.6% through OSSendMessage's readying a thread, 0.6% through alarms):
    Cemu's CounterSemaphore notify_all-ed on each 0 -> 1 of a run queue's count. Cores 0 and 2's host threads sleep
    there and were woken ~6,700 and ~4,900 times a second (context switches, against core 1's own ~1,500): ~190 real
    wakes a frame, ~4.5 us each.
  - 1.0% the scheduler lock's wake at its release (a taker slept at once, without a spin).
  - Page faults: 48 a second on the game thread, so not the write watch.
  - The syscall tracepoint (raw_syscalls:sys_enter) records in an app's profile without root (core 1: ~25,700 a
    second), but without its fields (no syscall numbers); the call chains named them instead.
- **The fix** (real time on three host threads; the single host thread of the checks never waits or contends there):
  - The run queues' counts are atomics that wake only a sleeper.
  - On arm64 a waiter first waits 100 us for an event (`ldaxr` arms the exclusive monitor on the count, `wfe` idles
    the core clock-gated until the queuer's store, the kernel's 10 kHz event stream for the deadline) before it sleeps.
  - The scheduler lock's taker spins 256 pauses (a few us) before its futex sleep.
  - Knobs for A/Bs: `WWHD_RUNQ_SPIN_US`, `WWHD_RUNQ_WFE`, `WWHD_LOCK_SPIN`.
- **A/B** (one APK, env switches, 20 s of simpleperf stat in the 60 part; off/pause/wfe/off/wfe):

  | | core 1 cycles | cores 0/2 cycles | cores 0/2 on CPU | cores 0/2 sleeps | fps |
  |---|---|---|---|---|---|
  | off | 42.8 G | 11.7 / 7.6 G | 4.6 / 3.0 s | 135k / 97k | 58.4-58.6 |
  | 100 us of pause | 41.4 G | 19.6 / 13.2 G | 7.6 / 5.1 s | 45k / 37k | 58.9 |
  | 100 us of wfe | 41.8 G | 12.8 / 8.9 G | 8.1 / 5.4 s | 48k / 32k | 58.8-58.9 |

  Core 1's cycles a frame: 36.6 -> 35.5 M (-3%); its busy share 81-82% -> 79-80%. The wfe wait spends a fifth of the
  pause loop's extra cycles, as the cores sit clock-gated while they wait (the device was charging, so no current
  was measured). An earlier run with a pause loop of 300 us: core 1 -4.5%, cores 0/2 +8 G.
- **x86** waits no time by default: no sleep is saved there, but only sleepers are woken (the mutex and notify_all go).
  A pause loop would cost a handheld's power (the Deck); AMD's MWAITX could do what wfe does.
- Left on the game thread: the runtime's 9.4% (psq_store, rt_journal_store's 60 fps journal, the HLE imports' wrappers,
  a `std::unordered_map<uint32, ...>` insertion in two game functions), the TLS resolver's 1.4%.
- **a4-cpu2, tried and not kept** (2026-10-15): the step rules' thread_locals through the context, as the store table
  is (ctx->rspTemp pointing at a per-host-thread struct of their addresses; RT_V_* in the emitted code and ppc_ops.h's
  helpers). Statically it took the generated program's resolver call sites from ~1,450 to ~130 (g_rtStep 1,146 -> 62,
  g_rtLateNotes 281 -> 0; what's left is the hand-written overrides'). On the Odin (sail at 60, on top of a4-codegen,
  A B B A, 20 s of simpleperf stat each) core 1 took 38.84 / 38.82 G cycles with it off and 39.87 / 39.79 G with it
  on: +2.6%. The resolver is called about once a function and its address kept, while each value through the context
  is three dependent loads (ctx->rspTemp, the struct, the value) taken again after every guest store, which may alias
  them: the static count of call sites overstated the resolver's cost. The code is on branch a4cpu2-parked.
- **After a4-cpu and a4-codegen** (9a523948, sail at 60, 40 s at 4000 Hz): core 1 is guest code 83.0%, our runtime
  9.4%, the kernel 3.9% (8.0% before a4-cpu: now 2.2% futex wakes from wakeupSingleThreadWaitQueue, the waits that
  outlast wfe's 100 us), the TLS resolver 1.7% (no caller over 0.2%), libc 1.1%. The runtime's leaves: rt_journal_store
  (60 fps's journal) ~1.1% in all, the actor maps' insertions (f_025DE58C, f_025D475C: s_knownActors and the stepping
  maps, sixty.cpp) 0.7%, GX2SetFetchShader's export wrapper 0.4%, the scheduler's switch 0.3%.
- **The wfe wait's length** (2026-10-15, on 9a523948 by env only: WWHD_RUNQ_SPIN_US; sail at 60, 20 s each, the
  battery discharging so its current_now was valid and steady):

  | wait | core 1 cycles | cores 0/2 cycles | core 1 busy | core 0 sleeps | battery |
  |---|---|---|---|---|---|
  | 100 us | 38.75 / 39.03 G | 12.8-12.9 / 8.4 G | 75-77% | 47k | 1,466 / 1,464 mA |
  | 250 us | 37.68 G | 13.1 / 8.3 G | 74-75% | 36k | 1,449 mA |
  | 500 us | 37.73 G | 13.6 / 8.6 G | 73-75% | 33k | 1,406 mA |

  A longer wait buys core 1's cycles (-3%) without power: the draw falls (fewer sleeps and wakes, the waiting cores
  clock-gated). 500 us is the default on arm64 now.

## A4 codegen (2026-10-14, session qa: a4-codegen)

Where the game core's time goes inside the generated code, on the arm64 worker (the Mac VM: a4prof.sh's run,
wwhd-play, WWHD_CORES=3, 2 minutes at 60): the profiler records each sample's leaf PC, so the core's samples map to
host instructions (scratch: the top 150 functions, objdump). Of the samples in them: stores into ctx 32%, loads from
ctx 23%, guest loads 13%, ALU 10%; by mnemonic ldr 36%, str 19%, strb 10.5%. By ctx field: the CR's bytes (cr0's four,
stored one by one on every compare) ~12%, remainingCycles 7.9% (5.1% the load at each function's entry; but the build
without any counting, WWHD_RT_NOCOUNT, is no faster: 7.38/7.41/7.18 against 7.38/7.25/7.31 ms a frame, as on x86 when
the real-time build landed, so those samples are the cost of entering a function, its fetch and the call, charged to
its first instructions), the FPRs 6.7%, LR and CTR ~3%; mfcr's 32-pass loop (f_02017D84/D98) 1.4%.

Also: in a large generated function clang's inliner stops at its size limit and calls shard-local copies of the
helpers (in 50 of the 156 shards ~12,500 calls to wr32, ~10,400 to the lfs conversion, ~5,900 to stfs's, ~7,600 to
wr16/wr8, ~1,200 to rt_fcmpu; x86 the same): a call per guest store or float access, and the guest registers read
again from ctx after it.

Changed (tools/recomp: ppc_ops.h, emit.py; every result as before, the fuzzer on both hosts, FTZ on and off, and the
checks): those helpers always inlined; lfs and stfs written from the ISA (a normal number or a zero widened by the
host, exact under any FTZ/DAZ mode; NaNs, infinities and denormals by the ISA's bit rules, out of line; Cemu's
conversions no longer called); fcmpu one host compare, its CR field one word store, the signalling-NaN test only when
unordered; every CR field write (compares, Rc=1, mtcrf) one word; mfcr eight bytes at a time. .text: arm64 116.6 ->
108.0 MB (-7.4%), x86 133.0 -> 120.9 MB (-9.1%). The Mac, 3 interleaved rounds of 2 minutes: the work a frame
7.48/7.49/7.45 -> 7.28/7.48/7.28 ms, the game thread 7.41/7.44/7.41 -> 7.30/7.43/7.28 (~-1.5%). x86 (the desktop
worker, the same run): the work a frame 4.39/4.27/4.42 -> 3.87/3.87/3.75 ms, the game thread 4.44/4.27/4.45 ->
3.90/3.88/3.75 (~-12%).

On the Odin (main 9a523948 against c93c45d1, route.sh sail at 60, A/B/A/B, simpleperf stat 20 s in the 60 part): the
game thread 41.91/42.17 -> 39.18/39.18 G cycles (-6.8%; 15.81/15.86 -> 15.26/15.25 s of CPU, the core clocked 2.65 ->
2.57 GHz), the scheduler thread 79-80% -> 76-77% busy, sail's late windows 58.5/58.8 and 59.1/58.4 -> 59.5/58.7 and
59.4/59.4 fps, the GPU thread 37.2/36.9 -> 36.4/36.0 G.

## A4 the GPU thread (2026-10-14, session bottom: a4-gpu)
- **Where the NullGPU thread's cycles go on the Odin** (top's 18:29 profile, 40 s, 70.4 G cycles; each sample put on
  the innermost function of ours on its stack, then split by where the leaf is: ours 36%, the driver 33%, libc 26%,
  the kernel 4%): RendererDraw's own calls 20.7% (the driver 10.9: binds, draws; libc 8.0: the copies),
  BeginRendering 13.6% (vkCmdBeginRendering: Qualcomm's driver allocates GPU memory at each, gsl_memory_alloc ->
  kgsl_sharedmem_alloc -> an ioctl), Descriptors 10.5% (ours 5.0, the driver's set allocation and update 4.6),
  BeginSlot 8.6% (the per-submit resets of the command buffer and the descriptor pool), SampleTexture 4.9%,
  ProgramHash 4.8%, processBuffer 4.8%, Submit 3.9%, packet 3.4%, hash map lookups ~4.5%, PipelineLayout 2.4%.
  (A scratch script over the NDK's simpleperf_report_lib; the profile and its output stay on the worker.)
- **What a frame uploads** (WWHD_RENDER_STATS=N now also prints, every N frames, a frame's bytes into the ring by kind,
  how much of each repeats an upload earlier in the ring's space, the binds, and the render passes with the calls that
  ended them; sail at 60, ~1,300 draws): uniform vars 860 KB (31% a repeat), uniform blocks 134 KB copied + 4.2 MB
  of zeros (92% a repeat: a dynamically indexed block takes a 64 KB range, filled past its bytes with zeros every
  draw), indices from the decode cache 1 MB (72% a repeat), vertices 6.1 MB (after the dedupe), no descriptor bind
  the last draw's, 413 KB of programs compared.
- **Kept: the ring's repeats reused** (draw.cpp): a uniform block whose address and sizes match an upload earlier in
  the ring's current space binds that copy when its bytes still equal it (memcmp of its bytes only, against a copy
  kept beside), and a decode-cache hit already copied into the ring this frame binds that copy. The draws read the
  same bytes: checks all MATCH, captures PSNR inf. Sail at 60: blocks 134 KB + 4.2 MB -> 20 KB + 316 KB a frame.
  On the Odin (sail, the system driver, gpustat.sh, A/B/A/B by WWHD_UBLOCK_REUSE=0 and WWHD_INDEX_REUSE=0): the
  NullGPU thread 34.99 / 35.04 G cycles in 20 s off, 34.60 / 34.35 on (-1.5%, both on below both off), fps the same
  (58.0-58.6): the zeros' memset was 2.8% of the thread in the profile.
- **Next**: the render passes. BeginRendering is 13.6% of the thread on Qualcomm's driver: fewer passes a frame
  (the counts and their ending calls are in the stats line now), passes that share their targets merged, and load
  and store ops the tiler needs no reload for; then the descriptor sets (10.5%) and BeginSlot's resets (8.6%).

## a4-rp4: the RP4's red scene (2026-10-15, session qa: B87)
- **What it was**: in play the owner's Retroid Pocket 4 Pro (Mali-G77, driver r32p1) showed a solid red scene. The HUD's
  layout shader (b6a47245ef5c6c50) draws some panes with w = 0 at every vertex (z 0 under its perspective projection).
  Such a primitive is outside the clip volume and the console draws nothing; Mali drew a 40x46 heart icon over the
  screen. Its inputs were the worker's, value for value (WWHD_RENDER_TRACE_VS); WWHD_RENDER_SKIP_VS on it showed the
  scene.
- **Why only there**: the RP4 has depthClamp but not VK_EXT_depth_clip_enable, and a clamped pipeline without the
  depth-clip struct isn't depth-clipped at all, so no draw was. Unclipped, Mali passes w <= 0 primitives on to the
  rasterizer; lavapipe still rejects them on the x/y planes, so a lavapipe run with the RP4's mask (depthClip) can't
  reproduce it (its captures differed only slightly: worst PSNR 40.87 dB). Making w slightly negative didn't help on
  Mali. The quick test on a device: WWHD_VK_MASK=depthClamp (the clamp off, so depth clipped).
- **Kept** (draw.cpp): on a device without the extension, a draw that clips depth goes without the clamp, so it's
  clipped as with the extension; a draw that doesn't keeps the clamp. Devices with the extension are unchanged.
  On the RP4: sail and the menus route right, the whole HUD. Known to lack the extension: the RP4's r32p1; the
  Odin's Qualcomm driver not checked.

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
