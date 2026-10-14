# Without Cemu (m-nocemu)

The owner's ask (plan.json `m-nocemu`, round 15): "remove all of cemu, being completely self reliant, using absolutely
nothing that has the cemu name attached to it". This file is the survey (2026-10-14, main 55410119; Cemu at the pinned
c717fcab with the 17 patches) and a proposed order of work. Nothing is changed yet.

## The decision on the licence (made)

**The owner, 2026-10-14: the MPL must not apply at all.** Every piece of Cemu-derived code is replaced by our own
implementation, not renamed or kept as a fork. That includes our 30 forks (`src/forks.txt`), the gx2 ports, the GPU
files derived from Latte and every other file whose header says it comes from Cemu (inventory C). At the end no file
in the tree is derived from Cemu's source, so no MPL notice remains. Two consequences:
- **No vendoring step.** Copying the rest of Cemu into our tree (so the build stops needing `/wwhd/opt/cemu-src`)
  would add ~133k MPL lines to the repo. Cemu stays an external source tree that shrinks phase by phase; the build
  stops reading it at N7.
- **`src/overrides/aspect.cpp`** (392 lines) is MPL-2.0 too, ported from the rival recomp (ZeldaWWHDRecomp), not
  from Cemu. Under "no MPL at all" it is rewritten as well.

The public git history keeps the old forks; nothing needs rewriting there.

## What "not derived" means in practice

- **Sources we may write from:**
  - the game itself: Ghidra on cking.rpx, its calls, structures and expectations;
  - our recorded oracle: traces, command streams, sound hashes, captures, layout logs. They are outputs of running the
    reference, not its code;
  - public documentation: WiiUBrew (RPL format, coreinit/IOSU structures), IBM's PowerPC 750CL manual (Espresso),
    AMD's R600/R700 ISA, register and PM4 packet documents, AMD's addrlib (MIT), Khronos specs.
- **Not sources:** Cemu's code (including our forks, the ports and `tools/reference/cemu-patches`). The same goes for
  other emulators under copyleft licences (decaf-emu GPL-3.0, Dolphin GPL-2.0+), which would bring a stronger licence
  with them. D14's "decaf-emu's SPIR-V translator is a second reference" goes too.
- **Facts carry over, expression doesn't.** What the guest can see may be reproduced exactly: structure offsets,
  return values, error codes, cycle charges, guest addresses of OS objects, PM4 opcode values (Cemu's own `IT_HLE_*`
  packets included). These are recorded behaviour. Code structure, names, comments and the wording of Cemu-specific
  algorithms must not carry over.
- **Process, per subsystem:** first a behaviour spec (`docs/specs/<area>.md`: what the guest sees, with evidence:
  trace excerpt, decomp address, document). Then code written from the spec with Cemu's file closed: new files, our
  names, our structure, no porting scripts (`tools/gx2_port.py` is the opposite). The recorded oracle decides. Each
  file's header says "Written from: <spec, documents>" and "Checked by: <gates>".
- **The limit.** The sessions have read Cemu's code for weeks, so a strict clean room isn't possible with the same
  sessions. The spec/code split and recorded sources are the practical standard. For the parts most at risk (below),
  the owner may want a session that never worked in that Cemu code to write it from the spec (open decision 3).

**Most at risk of coming out as copies** (exactness pulls the code towards Cemu's own expression):
1. **snd_core's mixer** (`ax_mix.cpp`, `ax_ist.cpp`, `ax_voice.cpp`: 3.4k lines). Cemu's own float formulas for
   sample-rate conversion, volume ramps, the biquad and upsampling, and a sound hash that needs every bit.
2. **The shader decompiler** (7.1k lines plus headers). GLSL shape, the uniform-layout contract, the exact-multiply
   helpers. Byte-identical captures pull towards identical GLSL.
3. **PPC floating point.** `tools/recomp/emit.py` says its semantics "mirror Cemu's interpreter ... statement for
   statement where the interpreter has quirks". Also the fres/frsqrte tables and quantize/dequantize it calls, and
   `ppc_ops.h`'s `rt_fcmpu` ("Cemu's fcmpu_espresso, inline").
4. **The scheduler** (`coreinit_Thread.cpp` and the 7 other coreinit forks): Cemu's own quantum, jitter, core and
   order, reproduced for the traces.
5. **gx2's packet writer** (16 ports and 4 core forks): the stream baseline is Cemu's packet choices and order.
6. **Lower risk: texture decoders and the address library.** Formats and tiling are hardware facts; AMD's addrlib
   (MIT) is a better source than Cemu's port of it.

## How it's checked

Every phase passes the usual gates, unchanged unless an open decision says otherwise:
- `tools/sixty/tests/checks.sh`: both routes' OS-call traces, GPU command streams and sound against
  `/wwhd/data/gx2/*-cemu.txt`, diff mode, and llvmpipe captures against g3;
- regress (its md5), statetest, blacktest, picto, bandam, predeploy, predeploy-rt.

The trace diff already compares by function name, not Cemu's HLE index (`hle_trace.py diff`), so our own dispatch can
keep the format. Two new gates:
- **The audit:** case-insensitive "cemu" in `src/`, `tools/` and the built binary (`nm`, `strings`), and files carrying
  an MPL notice. The counts must fall each phase and reach 0 at N8.
- **Provenance:** every source file has its "Written from" line.

## 1. Inventory: where the build still depends on Cemu

**A. Build plumbing.**
- `CMakeLists.txt`:
  - requires `CEMU_SRC` (`/wwhd/opt/cemu-src`) and `add_subdirectory`s it, with `ENABLE_WXWIDGETS OFF`;
  - takes vcpkg from Cemu's submodule (`dependencies/vcpkg/scripts/buildsystems/vcpkg.cmake`), its manifest
    (`WWHD_VCPKG_MANIFEST` = Cemu's `vcpkg.json`) and its overlay ports (`dependencies/vcpkg_overlay_ports*`).
- `src/CMakeLists.txt`:
  - filters Cemu's targets (CemuCafe, CemuCommon, CemuUtil, CemuConfig, CemuInput, CemuAudio, CemuComponents): Latte
    out except `latte_kept`, and our forks in place of the originals;
  - `wwhd_like_cemu()` gives all our object libraries CemuCafe's include paths, definitions and options,
    `${CEMU_SRC}/src/gui` and Cemu's precompiled header;
  - compiles Cemu's `main.cpp` and `mainLLE.cpp` as `wwhd_main`;
  - links with CemuBin's link options and libraries, plus CemuResource;
  - takes `imstb_truetype.h` from Cemu's `dependencies/imgui`;
  - on Android adds Cemu's `ELFSymbolTable.cpp` to CemuCommon; `wwhd_frontend` links CemuAudio for cubeb.
- `tools/arm64/build.sh` and `tools/android/build.sh` trim a copy of Cemu's `vcpkg.json` and turn Cemu's `ENABLE_*`
  options off. Their shims exist only for Cemu's CMake: `tools/arm64/cmake/FindGTK3.cmake`,
  `tools/android/cmake/FindX11.cmake`, `project.cmake` (X11 headers, a `log/log.h` stub).
- Workers: `tools/worker/setup-volume.sh cemu|cemu-rebuild` clones Cemu at c717fcab, applies the patches and builds
  `Cemu_release`. Its `sdl3` step reads Cemu's vcpkg download cache. The mac worker copies Cemu's `bin/`.

**B. Cemu code compiled into wwhd-null and wwhd-play** (x86-64 Linux, from Cemu's CMake lists with our filter): 274
of Cemu's own source files, about 133k lines, plus imgui's 4 (31.6k). By target:

| Target | Files | Lines | What, for us |
|---|---|---|---|
| CemuCafe | 191 | 98k | see the rows below |
| - coreinit (not forked) | 29 | 8.5k | heaps (MEM, Exp/Frm/Unit/Block), DynLoad, Init, Misc, Time, GHS, Atomic, MCP, OSScreen... |
| - gx2 originals | 16 | 5.5k | still compiled and registered; our ports take their table entries |
| - other OS libs | 61 | 30k | nn_* (act, acp, ac, boss, olv, ...), nlibcurl, nsysnet, sysapp, padscore, vpad, swkbd, erreula, nsyshid, nfc, h264, zlib125 |
| - Espresso | 32 | 21.5k | interpreter (5 files, 2.9k), Cemu's JIT (17, 15.4k: linked; `cpuMode = 0` leaves it unused), debugger and GDB stub (4, 2.1k), MMU and HW stubs |
| - IOSU | 14 | 8.2k | kernel, legacy act/acp/mcp/fpd/nim/crypto/ioctl, boss, PDM, ODM, NFC |
| - boot and files | 29 | 13.8k | CafeSystem, RPL loader (rpl.cpp 2.5k), GamePatch, fsc and its WUA/WUD/WUHB/host devices, FST, TitleList, GameProfile, graphic packs, Account |
| - Latte kept | 10 | 10.5k | shader decompiler (7.1k), address library (1.6k), FetchShader, GS copy shader parser, texture loader (+2.1k of decoders in its header) |
| CemuUtil | 19 | 6.9k | MemMapper (the 4 GB reservation), helpers, crypto, tinyxml2, Zir, IniParser |
| CemuCommon | 7 | 0.9k | exception handler, FileStream, cpu features, the precompiled header |
| CemuConfig | 4 | 1.2k | CemuConfig (settings.xml), ActiveSettings (paths), LaunchSettings (`-g`), NetworkSettings |
| CemuInput | 23 | 6.8k | InputManager and controllers (started, then stopped at boot) |
| CemuAudio | 4 | 0.8k | IAudioAPI (our SDL/AAudio/hash devices implement it), cubeb |
| CemuComponents | 18 | 12.8k | the logger (log.txt), PPC assembler, file cache, online services (napi, nex), Discord |
| CemuResource | 3 | 2.6k | the embedded fallback font, FontAwesome |
| wwhd_main | 2 | 0.3k | Cemu's `main()`: CemuCommonInit, the exception handler, `LaunchSettings`, `WindowSystem::Create()` (ours) |

Cemu's targets come together: CemuCafe links CemuCommon, CemuGui, ZArchive and imguiImpl, and Cemu's files call
its logger and config throughout. So no target can go before CemuCafe does, short of configure-time stand-ins (as
`gpu/vk/latte_glue.cpp` does for Latte). Also, our OS layer only overwrites HLE table entries that Cemu's
registration made (`os/install.cpp`). A Cemu library can't leave the build until ours registers its own entries.

**C. Our code derived from Cemu** (MPL headers; all to be rewritten): 51 files, 28.9k of our 59.1k lines of C++.
- **The 30 forks** in `src/forks.txt` (18.3k):
  - snd_core: `os/snd_core/ax_*.cpp`, 7 files;
  - the scheduler: `os/coreinit/coreinit_{Thread,Scheduler,Alarm,MessageQueue,ThreadQueue,Spinlock,
    Synchronization,Callbacks}.cpp`;
  - the file system: `coreinit_FS.cpp`, `coreinit_IPC.cpp`, `os/iosu/iosu_fsa.cpp`, `os/nn_save/nn_save.cpp`;
  - gx2's core: `os/gx2/core/GX2{,_Command,_Event,_Misc}.cpp`;
  - `os/proc_ui/proc_ui.cpp`, `os/tcl/TCL.cpp`, `os/common/SysAllocator.cpp`;
  - `runtime/espresso/PPC{Scheduler,Timer,InterpreterHLE}.cpp`, `runtime/fiber/FiberUnix.cpp`.
- **gx2's front half**, ported by `tools/gx2_port.py`: `os/gx2/*.cpp`, 16 files (5.6k).
- **The GPU side** (4.9k): `gpu/null_gpu.cpp` (from LatteCommandProcessor/Thread/Timing), `gpu/vk/draw.cpp` and
  `texture.cpp` (from Cemu's Vulkan renderer and texture code), `gpu/vk/latte_glue.cpp`.
- **`frontend/cemu_boot.cpp`** (from CemuApp/MainWindow).
- **Also:**
  - `tools/reference/cemu-patches/` (17 patches, 2.5k lines of diffs to Cemu's files);
  - `ppc_ops.h`'s inline `rt_fcmpu`, and `emit.py`'s mirrored semantics (risk 3 above);
  - the tools that copy Cemu code: `tools/cemu_fork.py`, `tools/gx2_port.py`.
- Our own files that match Cemu's results by behaviour (`os/input.cpp`, `swkbd.cpp`, `erreula.cpp`, `savestate.cpp`,
  ...) are ours. Review them for copied passages when their area is rewritten.

**D. Our code on Cemu's interfaces.**
- 77 of the 125 C++ files in `src/` and `tools/recomp/runtime` include Cemu headers (438 include lines). All 125
  compile with Cemu's precompiled header (`Common/precompiled.h`: the integer types, `betype`, `MEMPTR`, asserts,
  `fmt`, platform.h's Xlib on Linux).
- Most used:
  - `PPCInterpreter_t` (D2): 505 uses in 45 files, and every generated function;
  - `MEMPTR`/`MPTR`: 651 uses in 47 files; big-endian types: 605 in 37;
  - `cemuLog_*`: 316 uses in 53 files; `cemu_assert*`: 364 in 38;
  - Latte register and state names: 1,081 in 36; SysAllocator: 139 in 22; `osLib_*`: 199 in 18;
  - `ActiveSettings`/`GetConfig()`: 32 in 13 (paths, `tv_channels`, `tv_volume`, `console_language`);
  - `CafeSystem::`: 15 in 9; fsc: 59 in 4.
- `os/os.h` already gathers what our OS functions borrow (clock, current thread, sleep, callbacks, swap count, system
  area).
- **Mechanisms that are Cemu's:**
  - the execution seam `g_ppcExecuteHook` (patch 0011);
  - the HLE table and its trampolines `(1<<26)|index` (D4);
  - `WWHD_FORKS=0` with `runtime/without_forks.cpp`, `WWHD_OS=cemu`, `WWHD_AUDIO=cemu`;
  - the interpreter: still used in native mode for calls that don't land on a function entry, the trampoline glue
    and threads resuming mid-function, and as diff mode's oracle.

**E. Third-party code reached through Cemu.**
- vcpkg (Cemu's submodule, baseline f0fb3ddb) with Cemu's manifest: SDL3, zstd, zlib, libzip, rapidjson, glslang,
  fmt, boost (9 ports), pugixml, libpng, glm, openssl, curl, dbus, tiff, wxwidgets, libusb.
- Ours use SDL3 (vcpkg's on arm64 and Android; the worker's own build on x86), glslang, rapidjson, zstd, openssl
  (SHA in `game_files.cpp`), libzip (Android) and fmt. The rest serve Cemu's code.
- From Cemu's `dependencies/`, licences not Cemu's:
  - Vulkan-Headers (our `<vulkan/vulkan.h>` comes from there);
  - imgui (MIT; we use only `imstb_truetype.h`);
  - cubeb, ih264d, gamemode (headers), xbyak_aarch64 (Cemu's arm64 JIT);
  - ZArchive (MIT No Attribution, by Cemu's author, its own upstream repo): the `.wua` reader.

**F. Cemu runtime data read.**
- `WWHD_CEMU_DATA` (the `cemu/` folder of `tools/play/deploy.sh`, `app/cemu` in the APK from `tools/android/apk.sh`)
  holds Cemu's `resources/` and `gameProfiles/` (29 MB).
  - Only `resources/sharedFonts/Cafe{Cn,Kr,Std,Tw}.ttf` (27 MB) matters: `CafeSystem.cpp` loads them into the guest's
    shared-font area. The rest is Cemu's GUI translations.
  - `cemu/wwhd/shader_list.txt` is ours.
- In `portable/`:
  - `settings.xml` (CemuConfig; from `tools/reference/settings.xml`);
  - `gameProfiles/0005000010143500.ini` (`cpuMode = 0`);
  - `controllerProfiles/controller0.xml` (read only by Cemu's InputManager; our keys are in `os/input.cpp`);
  - `log.txt` (Cemu's logger and version string);
  - `mlc01/` (the NAND folders `CreateDefaultMLCFiles` makes; saves in `usr/save/...`).
- The game file is a `.wua`, Cemu's archive format. The Setup screen tells the player to "make a .wua of it with
  Cemu" (`frontend/pause_menu.cpp`).

**G. Names.**
- "cemu", any case: `src/` 1,213 lines in 101 files, `tools/` 474 in 104 (`tools/reference` 243), `docs/` 395.
- Files and dirs: `frontend/cemu_boot.cpp`, `tools/cemu_{fork,flags}.py`, `tools/reference/cemu-patches/`,
  `/wwhd/opt/cemu-src`, the baselines `/wwhd/data/gx2/*-cemu.txt`.
- 20 `CEMU_*` environment variables:
  - for the reference: `CEMU_BIN`, `CEMU_SRC`, `CEMU_PORTABLE`, `CEMU_COMMIT`;
  - read by our binary: `CEMU_INPUT_SCRIPT`, `CEMU_SHOT_FRAMES`/`_DIR`, `CEMU_SWKBD_AUTO`, `CEMU_HLE_TRACE*`,
    `CEMU_NO_GAMEPAD`, `CEMU_SYSALLOC_LOG`, `CEMU_VIRTUAL_CLOCK`, `CEMU_TEX_*`, `CEMU_DEBUG_ASSERT`, ...
- Our own variables that say Cemu: `WWHD_CEMU_DATA`, `WWHD_OS=cemu`, `WWHD_AUDIO=cemu`.
- In code: `wwhd_like_cemu()`, `cemuLog_log` and `cemu_assert` calls, "the Cemu area" (0x0E000000, where the OS
  objects live), `hle_trace.py`'s `mask_cemu_area`.
- `android/.../WindSongActivity.java` sets `WWHD_CEMU_DATA` and `CEMU_NO_GAMEPAD`.
- Cemu's target names (CemuCafe, CemuBin, ...).

**H. Real Cemu as the oracle.**
- The patched reference build (`Cemu_release`) is run by `tools/reference/run.sh`, `route.sh` (its default
  `CEMU_BIN`), `determinism.sh`, `survey.sh`, `shot.sh`, `press.sh` and `baseline.sh`.
- `tools/recomp/fuzz/build.sh` links Cemu's interpreter as the fuzzer's oracle. `tools/shaders/build.sh` links
  Cemu's decompiler.
- The stream and sound baselines (`/wwhd/data/gx2/*-cemu.txt`) were recorded with Cemu's objects (`WWHD_FORKS=0`,
  `WWHD_OS=cemu`), and the route traces with the reference build. The g3 captures are our renderer's.
- The gates read only recorded data; nothing in `checks.sh` runs Cemu. So the oracle is data, and it can be frozen.

## 2. Replacing each group

Sizes are new lines of our own, rough.

| Group | Replacing it means | Size | Risk to the checks | Needs first |
|---|---|---|---|---|
| Build, toolchain | our `vcpkg.json` and vcpkg checkout (same baseline); Vulkan-Headers from vcpkg; stb from upstream; no Cemu overlays or shims; then no `add_subdirectory`, `wwhd_like_cemu`, forks.txt | 0.3k | none | - |
| Base types, log, asserts | our header for integer, big-endian and guest-pointer types, our log (log.txt) and assert; files move off Cemu's PCH as their area is rewritten | 0.5k | none | - |
| Platform shell | our `main()` (no WindowSystem interface), settings and paths (no settings.xml, ActiveSettings, LaunchSettings, GameProfile), crash handler, audio sink interface (no IAudioAPI, cubeb), no InputManager; `cemu_boot.cpp` rewritten | 2-3k | low (values the OS reads, such as `tv_channels` and the language, must stay) | base |
| GPU side | own R700 shader translator (only the instructions WWHD uses: census), fetch- and GS-copy-shader parsers, texture decoders (WWHD's formats), address library (WWHD's tile modes, from AMD's addrlib); `null_gpu.cpp`, `draw.cpp`, `texture.cpp` rewritten, `latte_glue.cpp` gone | 12-16k | the address library and `null_gpu`'s effects are guest-visible (traces, streams); **captures will change** (decision 1); shader cache and list regenerated | base, own table entries (N1) |
| OS leaves | ours for every import Cemu still serves (the trace summary lists them): coreinit heaps, DynLoad, system info, misc, GHS, atomics; nn_act/acp/ac/boss/olv, nlibcurl, nsysnet, sysapp, the IOSU services they reach (act, acp, mcp, `SAVEInit`'s ioctl path); stubs for imports Cemu never had; each library's Cemu files then filtered out like Latte | 4-6k | medium: heap addresses and returned structures are in every trace | base, own table entries (N1) |
| Forked OS libraries | (a) snd_core, (b) gx2 front half, core and TCL, (c) proc_ui, (d) FS client, IPC, FSA, nn_save, and fsc's replacement (a `.wua` reader, saves on the host), (e) the OS-object layout re-keyed by our names at the recorded addresses (`sysalloc_layout.h` is keyed by Cemu file names today) | 15-18k | high: (a) sound hash bit for bit (decision 1), (b) stream byte for byte with Cemu's `IT_HLE_*` packets (decision 1), (d) the 41 save files; `savestate.cpp`'s serializers move with each | leaves |
| Kernel | threads, scheduler, alarms, queues, sync, callbacks; timer and timeslices; import dispatch and the HLETRC01 trace writer; fibers (from the SysV and AAPCS ABIs); our CPU context struct (D2 revised, the generator emits against it); our interpreter generated from `emit.py`'s semantics, with those rewritten from the 750CL manual and checked against recorded fuzz results | 6-8k | highest: thread order, core and timeslice boundaries are in every trace record | forked libs |
| Boot and memory | RPX loader (sections, relocations, imports bound by name, `code_patches.csv`), the 4 GB reservation and memory map, coreinit's init (main thread, stacks, heap arenas), the shared-font area | 3-4k | high: every guest address | kernel |
| Names, tools, docs | the renames (G); `cemu_fork.py`, `gx2_port.py`, `cemu_flags.py` deleted; fuzzer and shader tool against our build and recorded data; D2-D6, D10, D14, D18 rewritten | - | none | each phase |

Total: about 45-55k lines of our own, against today's 28.9k derived lines plus the ~133k of Cemu's that we link.
Ours can be smaller than Cemu's because it serves one game: 397 imported functions, ~245 of them called on the
routes.

## 3. Phases

Each phase keeps every gate green (or re-baselines one by an owner decision) before the next starts. N2 can run in
parallel with N3 and N4 (the bottom and top lanes).

1. **N0, the frozen oracle and the rules** (docs and data). Owner decisions taken. Every recorded reference listed,
   checksummed and on both workers. Newly recorded while Cemu still builds: the fuzzer's expected outputs per case
   as data (`FUZZ_DUMP` today keeps a hash), fres/frsqrte over their inputs and quantize/dequantize, and whether the
   game reads the shared fonts at all. Also: the spec template, the audit and provenance checks, D18 rewritten ("Cemu
   was the reference; its recorded outputs remain the oracle").
2. **N1, names and toolchain** (no behaviour change; every gate identical).
   - Our vcpkg manifest and checkout. Vulkan-Headers from vcpkg, stb from upstream.
   - `WWHD_*` names for the `CEMU_*` variables our binary reads. `cemu/` becomes `data/` and `WWHD_CEMU_DATA`
     becomes `WWHD_DATA`. Baselines renamed. "The Cemu area" becomes "the OS area".
   - Our base header, log and assert (forwarding to Cemu's while it's linked).
   - Our exports add their own HLE table entries, so Cemu's originals can leave the build. The trace diff goes by
     name, so the indices may move.
3. **N2, the GPU side** (bottom lane): own translator, parsers, decoders and address library; the four derived GPU
   files rewritten. The ported gx2 files (`surface`, `surface_copy`, `tiling_aperture`, `render_target`) switch to
   our address library: it is guest-visible, since gx2 writes the sizes, pitches and alignments it computes into the
   game's surfaces. Latte leaves CemuCafe entirely, and with it Cemu's gx2 originals (their handlers are already
   ours; they are the other callers of the address library).
   - Gates: traces, streams and sound identical. Captures by decision 1: compared within a tolerance against g3,
     reviewed, then a new g3.
4. **N3, the platform shell** (top lane): our `main()`, settings, paths, log file, crash handler and audio sink. Our
   code stops calling CemuConfig, ActiveSettings, LaunchSettings, IAudioAPI and InputManager. Cemu's remaining code
   gets its config values from ours through one bridge file.
   - The `.wua` reader per decision 5.
   - The Setup screen's text no longer sends players to Cemu.
5. **N4, the OS leaves** (top lane), one library at a time, each followed by removing its Cemu files from the build.
   - Gates: traces identical, plus the rest.
6. **N5, the forked libraries**, one step each: N5a snd_core, N5b gx2 and TCL, N5c proc_ui, N5d the file system
   and nn_save, N5e the OS-object layout. Gates: the sound hash (N5a), the stream (N5b), the 41 save files (N5d),
   statetest for each (the save-state format version goes up).
7. **N6, the kernel**: the scheduler and its friends, timer, timeslices, dispatch and trace writer, fibers. Then our
   CPU context struct and our own interpreter, so no `PPCInterpreter_t` is left.
   - Diff mode runs against our interpreter (decision 7). The fuzzer runs against the N0 data.
   - Gates: traces, diff mode, all the rest.
8. **N7, boot and memory**: our loader, memory map, coreinit init and shared font. Then `CMakeLists.txt` without
   `CEMU_SRC`, and vcpkg without what only Cemu used (boost, pugixml, curl, wxwidgets, tiff, libusb, dbus).
   - Gates: everything identical, built on a worker with no `/wwhd/opt/cemu-src`.
   - The binary's symbols and strings have no "cemu".
9. **N8, clean-up**: forks.txt, `without_forks.cpp`, `WWHD_FORKS`, `WWHD_OS`/`WWHD_AUDIO=cemu` and the copying
   tools deleted; `cemu-patches/` and the scripts that run the reference out of git (decision 2); `setup-volume.sh
   cemu`, the arm64 and Android shims and the manifest trimming gone; the APK and deploys with no `cemu/` folder;
   docs updated; the audit at 0.

Order rationale:
- Least coupled first. Cemu's remaining code is the interface everything else must still serve (`PPCInterpreter_t`,
  the HLE table, the scheduler's entry points, CafeSystem's boot), so the hub (N6) and the boot (N7) go last.
- Until then each rewrite exports, in one bridge file, the few names Cemu's remaining code calls.

## 4. The owner's decisions (2026-10-14)

- **Baselines: all new.** Where the oracle records Cemu's own inventions (the mixer's float formulas and the sound
  hash, the PM4 stream with its `IT_HLE_*` packets, the decompiler's GLSL and the captures), our rewrite sets a new
  baseline after a reviewed comparison (sound by ear and spectrum against the old recordings, the stream by what the
  GPU draws, captures by eye and PSNR against the old ones); nothing is reproduced from Cemu's arithmetic or encoding.
  The console-visible facts (what the guest reads back, structure layouts, call results) stay exact.
- **Clean room: spec first.** A written behaviour spec per subsystem, then code from the spec with Cemu's file
  closed, by the current sessions.
- **ZArchive: kept**, vendored from its own repository (MIT-0, not named Cemu).
- **Docs may mention Cemu as history** (research notes, old commits); nothing built, linked or shipped does.
- The rest as proposed below unless the owner says otherwise: the reference Cemu frozen after N0 and its patches moved
  out of the public repo; no GPL emulator code as a source; the shared font decided by N0's finding; diff mode decided
  after N6.

## 5. The open decisions as first written

1. **Exact to Cemu, or a new baseline?** In three places the oracle records Cemu's own inventions, not the console's:
   - the mixer's float formulas (the sound hash);
   - the PM4 stream with Cemu's `IT_HLE_*` packets (the stream baseline);
   - the decompiler's GLSL (byte-identical captures).
   Matching them exactly means reproducing Cemu's arithmetic or encoding from the recorded data. That is legal as
   facts, but it is where copying risk is highest. The alternative is a new baseline from our own code after a
   reviewed comparison. Proposal: keep the stream and sound exact (they're our strongest gx2 and snd_core checks);
   accept new captures after N2.
2. **The reference Cemu itself.** Keep the patched build on the worker volume (outside git and our build, never
   linked) to record what N0 misses, then freeze? Proposal: freeze after N0, and move `cemu-patches/` and the
   reference scripts out of the public repo into an archive on the worker.
3. **How strict a clean room.** One session writes from the spec with Cemu closed, or a session that never worked in
   that Cemu code writes it, for the mixer, translator, scheduler and FP semantics.
4. **Other emulators' code** (decaf-emu, Dolphin: GPL) as sources: proposal no, documents only.
5. **ZArchive** (MIT-0, its own repo, by Cemu's author, not named Cemu): vendor it from upstream, or our own `.wua`
   reader (~1k lines)? Also, which inputs to accept besides `.wua` (a decrypted dump folder is easy; WUD/WUX need
   keys), and what the Setup text tells players to use.
6. **The shared font** (Cemu's 27 MB `CafeStd.ttf` and siblings): ship a font we may ship at the same size, or none
   if N0 shows the game never reads it.
7. **Diff mode** after N6: keep it against our own interpreter (it then checks code generation, not semantics), or
   retire it in favour of traces plus the recorded fuzz data.
8. **"Named Cemu" in docs and history:** the product (binary, sources, shipped files, user-visible text) has none.
   May the docs still say that Cemu was the reference? (Proposal: yes, as history.)
