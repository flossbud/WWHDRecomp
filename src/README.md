# src - the wwhd executable

The product: Cemu's OS libraries plus our own frontend, and later our GX2 module and the
recompiled code (docs/recompiler-design.md, Architecture).

| Path | What |
|---|---|
| `frontend/window_system.cpp` | Cemu's `WindowSystem` interface: boot, one TV window (Xlib for now), event loop |
| `frontend/cemu_boot.cpp` | paths, config, default NAND files, title preparation (derived from Cemu's wx GUI, MPL-2.0) |
| `build.sh` | builds `build/wwhd/wwhd` on the worker against its Cemu build |

**Status (M0b.1, 2026-09-29):** `wwhd` replaces Cemu's wxWidgets GUI. Cemu's `main.cpp` still
parses the command line (`-g GAME`) and calls `WindowSystem::Create()`, which is ours. It links
Cemu_release's libraries minus `libCemuWxGui.a` with no unresolved symbols, boots WWHD on the
interpreter with Latte on the GPU, and its frames on the scripted route are byte-identical to
Cemu_release's. Run it through the reference scripts:

    tools/worker/job start wwhd-build src/build.sh
    tools/worker/job start wwhd-survey env CEMU_BIN=/wwhd/WWHDRecomp/build/wwhd/wwhd \
        tools/reference/survey.sh tools/reference/routes/title-to-game.txt /wwhd/data/shots/wwhd 1200 300

Next (M0b.2): our own CMake build of Cemu's Cafe sources without Latte, gx2 and TCL, and our GX2
module (exact front half, null backend) in their place; then the GX2 call stream to the title
screen must match the reference. The window moves to SDL3 there (Cemu's vcpkg SDL3 has no video).
