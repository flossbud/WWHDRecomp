# src - the wwhd executables

The product: Cemu's OS libraries plus our own frontend and GPU side, and later the recompiled
code (docs/recompiler-design.md, Architecture; D12 for the GPU split).

| Path | What |
|---|---|
| `frontend/window_system.cpp` | Cemu's `WindowSystem` interface: boot, one TV window (Xlib for now), event loop; headless with `WWHD_NULL_GPU` |
| `frontend/cemu_boot.cpp` | paths, config, default NAND files, title preparation (derived from Cemu's wx GUI, MPL-2.0) |
| `gpu/null_gpu.cpp` | null GPU in place of Latte: consumes gx2's command buffers, keeps the register file, performs every guest-visible effect, draws nothing (derived from Latte, MPL-2.0) |
| `build.sh` | builds `build/wwhd/wwhd` and `build/wwhd/wwhd-null` on the worker against its Cemu build |
| `link_order.py` | orders `wwhd-null`'s archive members like `wwhd`'s link (see below) |

    tools/worker/job start wwhd-build src/build.sh
    tools/worker/job start null-det env CEMU_BIN=/wwhd/WWHDRecomp/build/wwhd/wwhd-null \
        tools/reference/determinism.sh /wwhd/data/traces/null 600 tools/reference/routes/title-to-game.txt

**`wwhd`** (M0b.1) is Cemu's libraries with Latte on Vulkan, and our frontend instead of the
wxWidgets GUI. Cemu's `main.cpp` still parses `-g GAME` and calls `WindowSystem::Create()`, which
is ours. Its frames on the route are byte-identical to Cemu_release's.

**`wwhd-null`** (M0b.2) is headless and has no Latte: `libCemuCafe.a` is copied without the
objects built from `src/Cafe/HW/Latte`, except the address library, which gx2 needs for surface
layouts. Along the route its OS-call trace equals the reference's (59,531,239 calls to f600).

Link order matters. Static constructors run in link order, and each of Cemu's `SysAllocator`s
takes its slot in guest memory (0x0E000000 up) as it is constructed. Leaving Latte out changes
the order in which the linker pulls archive members, and with it host-side addresses the game
sees in registers. `build.sh` therefore links `wwhd-null` twice: once to learn which members it
needs, then with those members as explicit objects in `wwhd`'s order.

Next: the window moves to SDL3 (Cemu's vcpkg SDL3 has no video backends) when we have our own
CMake build; the Vulkan backend (D13) becomes a renderer on the null GPU's command processor.
