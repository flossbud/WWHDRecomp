# src - the wwhd executables

The product: Cemu's OS libraries plus our own frontend and GPU side, and later the recompiled
code (docs/recompiler-design.md, Architecture; D12 for the GPU split).

| Path | What |
|---|---|
| `frontend/window_system.cpp` | Cemu's `WindowSystem` interface: boot, one TV window (Xlib for now), event loop; headless with `WWHD_NULL_GPU` |
| `frontend/cemu_boot.cpp` | paths, config, default NAND files, title preparation (derived from Cemu's wx GUI, MPL-2.0) |
| `gpu/null_gpu.cpp` | null GPU in place of Latte: consumes gx2's command buffers, keeps the register file, performs every guest-visible effect, and hands draws, clears, copies and swaps to the renderer when it is on (derived from Latte, MPL-2.0) |
| `gpu/vk/renderer.cpp` | the Vulkan renderer (G2, `WWHD_RENDER=vk`): device, surfaces, clears, scan-out, frame capture, surface dumps |
| `gpu/vk/draw.cpp` | draws: shaders (Cemu's decompiler + glslang), pipelines, descriptors, uniform/vertex/index data, render targets (derived from Cemu's Vulkan renderer, MPL-2.0) |
| `gpu/vk/texture.cpp` | textures: from surfaces (render to texture, sized copies, mip chains) or decoded from guest memory with Cemu's texture loader; samplers (derived from Cemu, MPL-2.0) |
| `gpu/vk/latte_glue.cpp` | the pieces of Latte the decompiler calls, and a no-op `Renderer` (derived from Cemu, MPL-2.0) |
| `gpu/vk/vk.h`, `vk.cpp` | Vulkan loaded at runtime, so `wwhd-null` runs without a driver when rendering is off |
| `runtime/dispatch.cpp` | the execution seam (Cemu patch 0011): the hook that replaces Cemu's interpreter loop, the function table (D5), the D10 code check, `rt_call_ctr`/`rt_jump_ctr`/`rt_bad_branch` |
| `runtime/imports.cpp` | `rt_import`/`rt_import_data` (D4), bound from what Cemu's loader wrote into guest memory |
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

Next: the window moves to SDL3 (Cemu's vcpkg SDL3 has no video backends) when we have our own
CMake build, and the renderer presents to it.
