# src - the wwhd executables

The product: Cemu's OS libraries plus our own frontend and GPU side, and later the recompiled
code (docs/recompiler-design.md, Architecture; D12 for the GPU split).

| Path | What |
|---|---|
| `frontend/window_system.cpp` | Cemu's `WindowSystem` interface: boot, one TV window, event loop. `wwhd-null`: headless, or an SDL3 window with `WWHD_WINDOW=1` (the Xlib path was `wwhd`'s, which isn't built any more) |
| `frontend/audio_sdl.cpp` | the TV's sound on SDL3, as Cemu's audio device (windowed `wwhd-null`) |
| `frontend/overlay.cpp` | the system's keyboard, error dialogs and HOME sign, drawn over the game in the window |
| `frontend/cemu_boot.cpp` | paths, config, default NAND files, title preparation (derived from Cemu's wx GUI, MPL-2.0) |
| `gpu/null_gpu.cpp` | null GPU in place of Latte: consumes gx2's command buffers, keeps the register file, performs every guest-visible effect, and hands draws, clears, copies and swaps to the renderer when it is on (derived from Latte, MPL-2.0) |
| `gpu/vk/renderer.cpp` | the Vulkan renderer (G2, `WWHD_RENDER=vk`): device, surfaces, clears, scan-out, frame capture, surface dumps |
| `gpu/vk/present.cpp` | presenting the TV image to the window: swapchain, scaled and centred blit at every swap |
| `gpu/vk/draw.cpp` | draws: shaders (Cemu's decompiler + glslang), pipelines, descriptors, uniform/vertex/index data, render targets (derived from Cemu's Vulkan renderer, MPL-2.0) |
| `gpu/vk/texture.cpp` | textures: from surfaces (render to texture, sized copies, mip chains) or decoded from guest memory with Cemu's texture loader; samplers (derived from Cemu, MPL-2.0) |
| `gpu/vk/latte_glue.cpp` | the pieces of Latte the decompiler calls, and a no-op `Renderer` (derived from Cemu, MPL-2.0) |
| `gpu/vk/vk.h`, `vk.cpp` | Vulkan loaded at runtime, so `wwhd-null` runs without a driver when rendering is off |
| `os/` | our OS layer (D18): the game's imports, taking over Cemu's handlers one by one (`os.h`); `os/gx2/` is gx2, ported from Cemu's |
| `os/snd_core/`, `os/coreinit/`, `os/gx2/core/`, `os/proc_ui/`, `os/tcl/`, `os/iosu/`, `os/nn_save/`, `runtime/espresso/`, `runtime/fiber/` | forks of Cemu's sources (`forks.txt`): snd_core, the scheduler, gx2's core, proc_ui, TCL, the file system (its client, the IPC driver, the FSA service served in place) and nn_save, the cores' timeslices, timer and HLE dispatch, fibers |
| `runtime/dispatch.cpp` | the execution seam (Cemu patch 0011): the hook that replaces Cemu's interpreter loop, the function table (D5), the D10 code check, `rt_call_ctr`/`rt_jump_ctr`/`rt_bad_branch`, the real-time fast paths' switch and quiet watch (D19) |
| `overrides/` | overrides (D9): generated functions replaced by ours (`config/US_v0/overrides.txt`), the original still callable as `orig_f_X`; `task_loop.cpp`, the game's task loop, sleeps through idle rounds in real time |
| `runtime/imports.cpp` | `rt_import`/`rt_import_data` (D4), bound from what Cemu's loader wrote into guest memory |
| `runtime/profile.cpp` | a sampling profiler (`WWHD_PROFILE=path`): where host CPU time goes; `tools/profile_report.py` summarises it, `tools/reference/timing.sh` times a route without the trace (`WWHD_EXIT_FRAME=N` ends a run) |
| `runtime/write_watch.{h,cpp}` | page write-protection with stamps (`WWHD_WRITE_WATCH=1`, real time only; not wired in yet): which guest pages were written since a mark, a `SIGSEGV` handler chained before Cemu's, `HostWrite` scopes, a `sigaltstack` per host thread (`docs/research/texture-tracking.md`); unit tests: `runtime/tests/run.sh` |
| `os/tcl/tcl_host.h` | host waits on TCL (forked): the GPU thread sleeps until the CPU submits, the CPU until a submission retires |
| `runtime/diff.cpp` | diff mode (D8.2, M3): pure functions run natively, are rewound, and are compared (registers, stores, cycles) with the interpreter's run of the same call |
| `CMakeLists.txt` (and `../CMakeLists.txt`) | the build: Cemu's libraries from its source tree as a subproject, without Latte and with our forks in place of their originals; our sources in CemuCafe's compile context; `wwhd-null` linked like Cemu's own executable |
| `build.sh` | configures and builds `build/wwhd/wwhd-null` with CMake on the worker |
| `os/common/SysAllocator.cpp`, `sysalloc_layout.h` | Cemu's OS objects in guest memory (its SysAllocators), each at the reference's address (see below) |

    tools/worker/job start wwhd-build src/build.sh
    tools/worker/job start null-det env CEMU_BIN=/wwhd/WWHDRecomp/build/wwhd/wwhd-null \
        tools/reference/determinism.sh /wwhd/data/traces/null 600 tools/reference/routes/title-to-game.txt

**The build** is CMake's (`../CMakeLists.txt`, `CMakeLists.txt`); `build.sh` runs it with the
worker's settings. Cemu comes in as a subproject from `CEMU_SRC`, the same patched tree the reference
is built from (patch 0015 lets its CMake files work as a subproject), configured as Cemu's own build
is but without its wxWidgets GUI, its dependencies from vcpkg with Cemu's manifest (restored from
vcpkg's binary cache). The first build compiles Cemu's libraries too, 11-12 minutes with 10 jobs on
the worker; after that a change compiles what it touches and links once, with ThinLTO's cache
sparing most of the code generation: a one-file change takes about 10 s. Our sources compile as CemuCafe's own files do: its include
directories, definitions and options and those of its directories, and the usage requirements of
what it links, with Cemu's precompiled header, at -O2 and without LTO (the flags match what the old
`build.sh` gave them, apart from wxWidgets' and GTK's, which only Cemu's GUI uses). To build by hand:

    cmake -S . -B build/wwhd -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
    cmake --build build/wwhd -j 10

Options: `CEMU_SRC`, `WWHD_SDL3` (SDL3 with video for the window), `WWHD_RECOMP_DIR` and
`WWHD_FORKS`. The executable built with CMake gives the same traces, command streams and
sound as the one `build.sh` linked by hand before, on all six routes, and the same frames.

**The runtime** (M3) is linked into `wwhd-null`, together with the recompiled program when
`tools/recomp/build.sh` has generated it (`RECOMP_DIR`, default `build/recomp`; `RECOMP_DIR=` for
none); the build compiles it. The frontend installs Cemu's execution seam (`g_ppcExecuteHook`, cemu-patches/0011) before
launching the title. By default the hook runs Cemu's exact interpreter loop, so the trace stays
the reference's. With `WWHD_NATIVE=diff`, sampled calls of pure functions also run natively and are
checked against the interpreter (options in `runtime/diff.cpp`). **`WWHD_NATIVE=on` runs the
recompiled program** (M4): its whole-route trace equals the reference's, in half the interpreter's
time. `WWHD_RT_LOG=path` collects the runtime's log (and, in diff mode, per-function results in
`path.funcs.csv` at exit).

**Overrides** (`overrides/`, D9) replace generated functions: `config/US_v0/overrides.txt` lists
them, `tools/recomp/generate.py` emits their bodies as `orig_f_X`, and the build compiles
`overrides/*.cpp` like generated code and links them with it. Two so far, both real-time only. The GamePad's screen (`gamepad_view.cpp`, `f_027D6BB0`): the
game draws its ITEMS menu for the absent GamePad every frame, 9% of the draws; in real time a view
whose render target is the GamePad's 854x480 screen isn't drawn (`WWHD_SKIP_GAMEPAD=1` forces it,
`=0` never). And the game's task
loop (`f_0275FFCC`), a **real-time fast path** (D19): with the virtual clock, in diff mode or on
Cemu's three host threads it runs the game's code (`orig_f_X`), so every check sees the game's own
behaviour; in real time on one host thread it sleeps through the rounds where the game's ticking
task only posts itself its tick again (the scheduler thread went from 100% busy to 8-25% on the save
route). `WWHD_FAST_PATHS=0` turns the fast paths off, to compare; `WWHD_QUIET_DEBUG=n` logs why the
first n watched calls didn't count as idle.

The null GPU also collects the G0 draw statistics (design D15) with `WWHD_GPU_STATS=path`: the
register state at every draw (programs by content, targets, depth, MSAA, geometry shaders,
stream-out, primitive types), with the pipeline variants in `path.variants.csv`. With
`WWHD_GPU_DUMP=dir` it also writes, once each, every program the GPU runs and, for each new variant,
the whole register file at its first draw: the inputs `tools/shaders/translate` needs (G1).

    tools/worker/job start recomp-build tools/recomp/build.sh
    tools/worker/job start wwhd-build src/build.sh
    tools/worker/job start m3-diff env WWHD_NATIVE=diff WWHD_RT_LOG=/wwhd/data/traces/m3/rt.log \
        CEMU_BIN=/wwhd/WWHDRecomp/build/wwhd/wwhd-null tools/reference/route.sh /wwhd/data/traces/m3 \
        10800 tools/reference/routes/title-to-game.txt /wwhd/data/traces/null-route.zst

At its first timeslice the runtime checks the linked code against guest memory: each function's
hash against the RPX's (a mismatch means Cemu's GamePatch rewrote it; it then stays
interpreted), every import's relocated branch sites (all must lead to one trampoline), every
relocated data-import immediate, and, in diff mode, its store decoder against the generator's
store census. Any disagreement stops the run.

**`wwhd`** (M0b.1) was Cemu's libraries with Latte on Vulkan and our frontend; its frames were
byte-identical to Cemu_release's. Since WW-3's CMake build it isn't built: Cemu_release itself is the
comparison. Cemu's `main.cpp` still parses `-g GAME` and calls `WindowSystem::Create()`, which is
ours.

**`wwhd-null`** (M0b.2) has no Latte: Cemu's CemuCafe target loses the sources in
`src/Cafe/HW/Latte` at configure time, except the address library, which gx2 needs for surface
layouts, and the pieces the renderer uses (below). Along the route its OS-call trace equals the reference's: 59,531,239 calls to f600, and
all 1,124,796,468 calls over the whole route into gameplay, which takes 12 minutes, about 2x
faster than the reference on the GPU.

**Cemu's OS objects** (threads, queues, IPC buffers, the sound and GPU state the game sees) are
its `SysAllocator`s: 170 slots in the Cemu area (0x0E000000 up). Cemu lays them out in the order
their static constructors register them, which is the link's, so leaving Latte out (or any other
change to the link) moved them, and with them addresses the game sees in registers and traces.
Ours go where the reference has them: `os/common/SysAllocator.cpp` (a fork) places each slot at
the offset `os/common/sysalloc_layout.h` gives it, by what the slot is (the file that declares it,
with the line for a header, its size and alignment, and how many alike that file declared before
it: cemu-patches/0016 has each slot record where it was declared), then lets the area's bump
allocator carry on from where the reference's did. A slot the table doesn't know goes after the
reference's, and the log says so. So the link order doesn't matter: one plain link gives guest
memory exactly as the reference's (all six routes and diff mode identical). Make the table again
when Cemu or a fork's slots change: `CEMU_SYSALLOC_LOG=path` (patch 0016) lists the slots as laid
out by a run that matches the reference (the reference itself writes one equal to the table), and
`tools/sysalloc_layout.py path > src/os/common/sysalloc_layout.h`. `WWHD_SYSALLOC_ORDER=link`
falls back to Cemu's layout by registration order. Before the table, the build linked twice,
the second time with every archive member in the reference's order.

**The file system** (D18) is forked whole: the FS client (`os/coreinit/coreinit_FS.cpp`: clients,
command blocks and their queue, the FS API), the IPC driver (`os/coreinit/coreinit_IPC.cpp`), the
FSA service (`os/iosu/iosu_fsa.cpp`) and `os/nn_save/nn_save.cpp`. Cemu ran the FSA service on an
IOSU host thread behind IOSU's kernel; ours is served in place (`os/iosu/fsa_service.h`): the client
serves each request on the guest thread that made it, then hands the reply to its core's IPC thread
as IOSU's kernel did, with a message sent the way a host thread sends one (it readies the IPC
thread, never switches to it: `__OSSendMessageAsHost`). With the virtual clock Cemu delivered the
reply before `IOS_IoctlAsync` returned (cemu-patches/0003), so the guest sees the same steps: the
caller blocks on its command block's queue, the IPC thread runs the FS callback, the caller wakes.
Synchronous requests (`IOS_Open`, `IOS_Ioctl`) never blocked and are plain calls now. The service's
handles are its own (a client's index plus one). Files are still Cemu's `fsc` (title content from
a WUA archive or a folder, the save folder on the host). Checked: all six routes as the reference's,
the 41 save files the new-game route writes byte-identical to those Cemu's FS writes, and a
real-time run at 30 fps.

**Finding which game function a call belongs to:** `WWHD_BACKTRACE=lib.Function[:rN=value]` logs,
for the first `WWHD_BACKTRACE_COUNT` (default 4) calls of that import (whose rN holds value, if
given), the guest's return addresses from LR and the stack's back chain
(`runtime/imports.cpp`; e.g. `gx2.GX2CopyColorBufferToScanBuffer:r4=4`, the GamePad's copy).
`WWHD_SHOT_DRC=1` also writes the GamePad's image at each capture (`fNNNNNN.drc.ppm`).

**Saves** live where Cemu keeps them: `portable/mlc01/usr/save/00050000/10143500/user/80000001/`
(`cking.sav`, the Pictograph photos `cking_pic*.sav`, `cking_playlog.sav`). The game reads and
writes them through Cemu's `nn_save` and FS, so saving and loading work as on the console. To start
from an existing save, copy its files there; `tools/reference/run.sh` does it with `REF_SAVE=dir`
(after `REF_FRESH` wipes the NAND).

**Our OS layer** (`os/`, design D18) implements the game's imports one at a time, replacing Cemu's.
At the first timeslice `os::Install` writes each function over its import's entry in Cemu's HLE
table, so Cemu's dispatch still traces the call and charges its cycles, and every function must
leave registers and guest memory exactly as Cemu's did: both route traces stay identical. What a
function still needs from Cemu (the clock, the current thread) goes through an accessor in
`os/os.h`. A library whose functions share state with each other moves whole. `WWHD_OS=cemu`
leaves every handler Cemu's; the log says how many are ours. Libraries the game loads itself
(`swkbd`, `erreula`) register their handlers when it does, so `OSDynLoad_Acquire` is wrapped to take
those over as they appear. So far 255: memory and cache operations, the clock, thread-specific slots
and errno, the interrupt mask, a few system flags, the stateless parts of `nn_ac` and `nn_act`,
input (`os/input.cpp`: `padscore` with one Pro Controller, `vpad` with no GamePad), the software
keyboard (`os/swkbd.cpp`), the error dialogs (`os/erreula.cpp`) and 179 of gx2's (`os/gx2/`). The
gx2 files are Cemu's, ported by `tools/gx2_port.py` (MPL-2.0; our namespace and registration;
`os/gx2/misc.cpp` by hand), and still send their commands through Cemu's command pipe. What stays
Cemu's is gx2's core: the command pool and display lists, flush, GPU timestamps and waits, vsync,
swap and GX2Init (3.7% of the route's gx2 calls), which rests on the scheduler and Cemu's TCL ring.
Every gx2 change must leave the GPU command stream exactly as Cemu's gx2 sent it:
`tools/reference/stream_check.sh save|route NAME` checks the trace and a per-frame hash of every
packet the GPU executes (`WWHD_GPU_STREAM=path`) against baselines recorded with Cemu's gx2
(45,955,744 packets on the save route, 143,098,119 on the whole route), and the sound: a hash of
every mixed block (`WWHD_AUDIO_HASH=path`, a device that plays nothing; 5,180 blocks on the save
route, 30,219 on the whole route).

**Forks.** Libraries that Cemu's own code calls into can't be taken over at the HLE table (the
scheduler calls snd_core's `AXOut_update` directly; CafeSystem and coreinit call gx2's core). Those
are forked instead: our copy of Cemu's source file, under Cemu's names, which the build links in
place of Cemu's object. `src/forks.txt` lists them (29 so far: snd_core; the scheduler, i.e.
coreinit's threads, scheduler, alarms, message and thread queues, spinlocks, synchronization and
callbacks, and the Espresso timeslices and timer with the virtual clock; gx2's core; proc_ui; TCL;
the HLE dispatcher and trace recorder; fibers, with our own context switch; the file system's
client, the IPC driver and the FSA service; nn_save) and
`tools/cemu_fork.py` creates missing ones. Each takes its original's place in the Cemu target
that had it, so it compiles exactly as Cemu compiles that file (ThinLTO bitcode included); its
SysAllocators keep the reference's addresses through the layout table (above). A
fork is ours to change from then on (Cemu patches in `tools/reference/cemu-patches` no longer reach
it); `WWHD_FORKS=0 src/build.sh build/wwhd-cemu` builds with Cemu's objects instead, to record
baselines (what the rest of wwhd-null takes from the forks has weak stand-ins there,
`runtime/without_forks.cpp`; it reproduces the save route's command stream and sound exactly). Input comes from the input
script when `CEMU_INPUT_SCRIPT` names one (the reference's format, so routes replay identically),
otherwise from the window's keyboard and gamepad. The keyboard answers itself when
`CEMU_SWKBD_AUTO` gives a name (as the reference's does; `tools/reference/run.sh` defaults to
`Link`, `CEMU_SWKBD_AUTO=` leaves it to the player); otherwise the window shows it and the player
types. The system draws the keyboard and error dialogs, not the game, so the frontend does
(`frontend/overlay.cpp`), over the TV image in the window and never into captures.

**The renderer** (G2, design D13 as built) is off unless `WWHD_RENDER=vk`. It needs a Vulkan 1.3
device with dynamic rendering (lavapipe: `REF_GPU=llvmpipe` in `tools/reference/run.sh`). It draws on
the GPU thread and never writes guest memory, so the trace is the same with it on or off. Options:
`CEMU_SHOT_FRAMES`/`CEMU_SHOT_DIR` capture the TV image as the reference does (`survey.sh` sets
them; shot N is the image the (N+1)th swap presents, as in the reference), `WWHD_RENDER_STATS=N`
logs draw counts every N frames, `WWHD_RENDER_DUMP=N` writes every surface at the Nth swap as PPM
into the shot directory (compare with the reference's using `tools/reference/compare_dumps.py`),
`WWHD_RENDER_TRACE=N:ADDR[:X,Y]` logs every draw into the color target at ADDR in frame N (state,
programs, texture sources) and, with a pixel, which draw changed it (N=0: every frame, pixel
changes only; ADDR 0: every draw of frame N, with its targets, the area it draws and its textures), `WWHD_RENDER_SHADERS=dir` writes every shader's GLSL as `<key>.<vs|ps>.glsl` (keys
as the trace prints them; game-derived, keep it out of git), and `WWHD_RENDER_COPIES=1` reports
GX2's GPU-side surface copies. Dumps write every array layer, colour as 8-bit PPM and depth and
single-channel float as 16-bit PGM, like the reference's (cemu-patches/0012).

    tools/worker/job start g2-vk env CEMU_BIN=/wwhd/WWHDRecomp/build/wwhd/wwhd-null REF_GPU=llvmpipe \
        WWHD_RENDER=vk WWHD_NATIVE=on tools/reference/survey.sh tools/reference/routes/title-to-game.txt \
        /wwhd/data/g2/vk 600 30 0
    tools/worker/w uv run -q tools/reference/compare_frames.py /wwhd/data/g2/ref /wwhd/data/g2/vk

For the renderer `libCemuCafe_wwhd.a` (Cemu's, without Latte) also keeps Latte's shader decompiler, fetch- and
GS-copy-shader parsers and texture loader; `gpu/vk/latte_glue.cpp` stands in for the rest of Latte
they call.

**The window** (`WWHD_WINDOW=1`, with `WWHD_RENDER=vk`) is SDL3's, on X11 or Wayland. At every
swap the renderer blits the TV image into it, scaled to fit and centred, with the bytes the capture
has (the swapchain takes the scan buffer's sRGB encoding). F11 or Alt+Enter toggles fullscreen,
`WWHD_VSYNC=1` asks for FIFO presentation instead of mailbox. Cemu's vcpkg SDL3 has no video, so
`wwhd-null` links our build of the same version (`tools/worker/setup-volume.sh sdl3`) in its place;
the traces are unchanged. The window's keyboard and first gamepad are the Pro Controller: keys as
the reference's `controller0.xml` (A=X B=Z X=S Y=A L=Q R=W ZL=1 ZR=2 +=Return -=Backspace, D-pad
on the arrows, left stick I/J/K/L, right stick T/F/G/H), gamepad face buttons by their printed
label, triggers as ZL/ZR, and rumble. While the keyboard or an error dialog is up the game gets no
input: typing goes to the keyboard (Enter or Start for OK), A and B answer a dialog. The TV's
sound plays through SDL3 too (`frontend/audio_sdl.cpp`, fed by the AX mixer); `WWHD_AUDIO_HASH`
hashes it instead, and headless runs without either are silent. `WWHD_AUDIO=cemu` uses Cemu's
cubeb device instead (so does the window when SDL's audio fails). **Cemu's idle threads are
stopped at start-up:** its input manager (an update thread waking every millisecond, and the
Wiimote and SDL controller providers' threads; all of the game's input calls are ours) and, unless
its device is wanted, its audio backend (cubeb's PulseAudio thread). In real time the GPU thread
sleeps until a submission or the host-timed vsync (`untilTimedVsync`), no longer waking every
millisecond. On the worker that took the process from 62% of a core and 4,500 wakeups a second to
55% and 800, leaving the scheduler thread and the GPU thread (traces, command streams and sound
unchanged). On
the worker, with lavapipe under Xvfb, the save route runs windowed at about 5 frames a second, and
its trace stays the reference's with the window, input and sound on (`SDL_AUDIO_DRIVER=disk` writes
the sound to a file):

    tools/worker/job start win env CEMU_BIN=/wwhd/WWHDRecomp/build/wwhd/wwhd-null WWHD_NATIVE=on \
        REF_GPU=llvmpipe WWHD_RENDER=vk WWHD_WINDOW=1 REF_SAVE=/wwhd/data/saves/wwhd_100 \
        tools/reference/route.sh /wwhd/data/traces/win 1200 tools/reference/routes/continue-100.txt

**Playing it** on a desktop with a GPU: `tools/play/deploy.sh HOST` (from the editing machine, after
`src/build.sh`) streams the build, Cemu's data files, the game and the test saves from the worker to
`~/wwhd-play` on HOST, and `~/wwhd-play/play.sh` runs it windowed in real time (its header lists the
keys and switches; `WWHD_SAVE=saves/wwhd_100` starts from the 100% save). **Real time** is every run
without the virtual clock (design D19): guest time from `steady_clock`, the scheduler thread sleeping
when no guest thread can run, the null GPU sleeping until the host-timed vsync, and every 10 s a
line in `portable/log.txt` with the frame rate, frame times, how busy the scheduler thread was and
the shaders and pipelines seen for the first time. **The shader cache** (design D20,
`gpu/vk/shader_cache.cpp`) keeps every translated shader and pipeline recipe in
`portable/shaderCache/wwhd` and builds them all before the game starts, with a "Preparing shaders"
screen in the window when that takes more than 0.3 s (`WWHD_SHADER_THREADS=n` sets the threads);
`tools/reference/shader_cache_check.sh` shows it changes nothing on screen. **The shader list**
(`gpu/vk/shader_list.cpp`, `config/US_v0/shader_list.txt`, which `tools/play/deploy.sh` puts in
`cemu/wwhd/`) makes a first start the same: whatever playthroughs have translated and built that the
cache doesn't have yet is translated before the game starts, from the programs in the player's own
game files and the registers the list keeps (`WWHD_SHADER_LIST=path`, or `none`).
`WWHD_SHADER_SOURCES=path` makes a run record everything it meets for the first time, for
`tools/shaders/shader_list.py` to merge into the list (such a run prepares nothing, so it meets
everything); `tools/reference/shader_list_check.sh` checks a first start from the list.
