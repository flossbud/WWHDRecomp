# src - the wwhd executables

The product: Cemu's OS libraries plus our own frontend and GPU side, and later the recompiled
code (docs/recompiler-design.md, Architecture; D12 for the GPU split).

| Path | What |
|---|---|
| `frontend/window_system.cpp` | Cemu's `WindowSystem` interface: boot, one TV window (Xlib for now), event loop; headless with `WWHD_NULL_GPU` |
| `frontend/cemu_boot.cpp` | paths, config, default NAND files, title preparation (derived from Cemu's wx GUI, MPL-2.0) |
| `gpu/null_gpu.cpp` | null GPU in place of Latte: consumes gx2's command buffers, keeps the register file, performs every guest-visible effect, draws nothing (derived from Latte, MPL-2.0) |
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
layouts. Along the route its OS-call trace equals the reference's: 59,531,239 calls to f600, and
all 1,124,796,468 calls over the whole route into gameplay, which takes 12 minutes, about 2x
faster than the reference on the GPU.

Link order matters. Static constructors run in link order, and each of Cemu's `SysAllocator`s
takes its slot in guest memory (0x0E000000 up) as it is constructed. Leaving Latte out changes
the order in which the linker pulls archive members, and with it host-side addresses the game
sees in registers. `build.sh` therefore links `wwhd-null` twice: once to learn which members it
needs, then with those members as explicit objects in `wwhd`'s order.

Next: the window moves to SDL3 (Cemu's vcpkg SDL3 has no video backends) when we have our own
CMake build; the Vulkan backend (D13) becomes a renderer on the null GPU's command processor.
