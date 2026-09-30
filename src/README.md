# src - the wwhd executables

The product: Cemu's OS libraries plus our own frontend and GPU side, and later the recompiled
code (docs/recompiler-design.md, Architecture; D12 for the GPU split).

| Path | What |
|---|---|
| `frontend/window_system.cpp` | Cemu's `WindowSystem` interface: boot, one TV window, event loop. `wwhd`: Xlib. `wwhd-null`: headless, or an SDL3 window with `WWHD_WINDOW=1` |
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
| `os/snd_core/`, `os/coreinit/`, `os/gx2/core/`, `os/proc_ui/`, `runtime/espresso/` | forks of Cemu's sources (`forks.txt`): snd_core, the scheduler, gx2's core, proc_ui, the cores' timeslices and timer |
| `runtime/dispatch.cpp` | the execution seam (Cemu patch 0011): the hook that replaces Cemu's interpreter loop, the function table (D5), the D10 code check, `rt_call_ctr`/`rt_jump_ctr`/`rt_bad_branch` |
| `runtime/imports.cpp` | `rt_import`/`rt_import_data` (D4), bound from what Cemu's loader wrote into guest memory |
| `runtime/profile.cpp` | a sampling profiler (`WWHD_PROFILE=path`): where host CPU time goes; `tools/profile_report.py` summarises it |
| `runtime/diff.cpp` | diff mode (D8.2, M3): pure functions run natively, are rewound, and are compared (registers, stores, cycles) with the interpreter's run of the same call |
| `build.sh` | builds `build/wwhd/wwhd` and `build/wwhd/wwhd-null` on the worker against its Cemu build |
| `link_order.py` | orders `wwhd-null`'s archive members like `wwhd`'s link (see below) |

    tools/worker/job start wwhd-build src/build.sh
    tools/worker/job start null-det env CEMU_BIN=/wwhd/WWHDRecomp/build/wwhd/wwhd-null \
        tools/reference/determinism.sh /wwhd/data/traces/null 600 tools/reference/routes/title-to-game.txt

**The runtime** (M3) is linked into `wwhd-null`, together with the recompiled program when
`tools/recomp/build.sh` has built it (`RECOMP_DIR`, default `build/recomp`; `RECOMP_DIR=` for
none). The frontend installs Cemu's execution seam (`g_ppcExecuteHook`, cemu-patches/0011) before
launching the title. By default the hook runs Cemu's exact interpreter loop, so the trace stays
the reference's. With `WWHD_NATIVE=diff`, sampled calls of pure functions also run natively and are
checked against the interpreter (options in `runtime/diff.cpp`). **`WWHD_NATIVE=on` runs the
recompiled program** (M4): its whole-route trace equals the reference's, in half the interpreter's
time. `WWHD_RT_LOG=path` collects the runtime's log (and, in diff mode, per-function results in
`path.funcs.csv` at exit).

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

**`wwhd`** (M0b.1) is Cemu's libraries with Latte on Vulkan, and our frontend instead of the
wxWidgets GUI. Cemu's `main.cpp` still parses `-g GAME` and calls `WindowSystem::Create()`, which
is ours. Its frames on the route are byte-identical to Cemu_release's.

**`wwhd-null`** (M0b.2) is headless and has no Latte: `libCemuCafe.a` is copied without the
objects built from `src/Cafe/HW/Latte`, except the address library, which gx2 needs for surface
layouts, and the pieces the renderer uses (below). Along the route its OS-call trace equals the reference's: 59,531,239 calls to f600, and
all 1,124,796,468 calls over the whole route into gameplay, which takes 12 minutes, about 2x
faster than the reference on the GPU.

Link order matters. Static constructors run in link order, and each of Cemu's `SysAllocator`s
takes its slot in guest memory (0x0E000000 up) as it is constructed. Leaving Latte out changes
the order in which the linker pulls archive members, and with it host-side addresses the game
sees in registers. `build.sh` therefore links `wwhd-null` twice: once to learn which members it
needs, then with those members as explicit objects in `wwhd`'s order.

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
are forked instead: our copy of Cemu's source file, under Cemu's names, which `build.sh` links in
place of Cemu's object. `src/forks.txt` lists them (22 so far: snd_core; the scheduler, i.e.
coreinit's threads, scheduler, alarms, message and thread queues, spinlocks, synchronization and
callbacks, and the Espresso timeslices and timer with the virtual clock; gx2's core; proc_ui) and
`tools/cemu_fork.py` creates missing ones. They compile exactly as Cemu compiles them (its flags for
that file, ThinLTO bitcode) and `link_order.py` puts them at Cemu's objects' positions, so static
constructors, and the guest-memory slots of their SysAllocators, keep their order and addresses. A
fork is ours to change from then on (Cemu patches in `tools/reference/cemu-patches` no longer reach
it); `WWHD_FORKS=0 src/build.sh build/wwhd-cemu` builds with Cemu's objects instead, to record
baselines. Input comes from the input
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
changes only), `WWHD_RENDER_SHADERS=dir` writes every shader's GLSL as `<key>.<vs|ps>.glsl` (keys
as the trace prints them; game-derived, keep it out of git), and `WWHD_RENDER_COPIES=1` reports
GX2's GPU-side surface copies. Dumps write every array layer, colour as 8-bit PPM and depth and
single-channel float as 16-bit PGM, like the reference's (cemu-patches/0012).

    tools/worker/job start g2-vk env CEMU_BIN=/wwhd/WWHDRecomp/build/wwhd/wwhd-null REF_GPU=llvmpipe \
        WWHD_RENDER=vk WWHD_NATIVE=on tools/reference/survey.sh tools/reference/routes/title-to-game.txt \
        /wwhd/data/g2/vk 600 30 0
    tools/worker/w uv run -q tools/reference/compare_frames.py /wwhd/data/g2/ref /wwhd/data/g2/vk

For the renderer `libCemuCafe_nolatte.a` also keeps Latte's shader decompiler, fetch- and
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
input: typing goes to the keyboard (Enter or Start for OK), A and B answer a dialog. Cemu's SDL
controller provider is dropped when the window opens: its thread would otherwise take events off
SDL's queue. The TV's sound plays through SDL3 too (`frontend/audio_sdl.cpp`,
fed by Cemu's AX mixer until `snd_core` is ours; `WWHD_AUDIO=cemu` keeps Cemu's Cubeb device). On
the worker, with lavapipe under Xvfb, the save route runs windowed at about 5 frames a second, and
its trace stays the reference's with the window, input and sound on (`SDL_AUDIO_DRIVER=disk` writes
the sound to a file):

    tools/worker/job start win env CEMU_BIN=/wwhd/WWHDRecomp/build/wwhd/wwhd-null WWHD_NATIVE=on \
        REF_GPU=llvmpipe WWHD_RENDER=vk WWHD_WINDOW=1 REF_SAVE=/wwhd/data/saves/wwhd_100 \
        tools/reference/route.sh /wwhd/data/traces/win 1200 tools/reference/routes/continue-100.txt
