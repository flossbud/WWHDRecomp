# Handoff: WWHD recomp, state as of 2026-09-29

Read this first, then `CLAUDE.md`, `docs/recompiler-design.md` (decisions D1–D17, milestones,
status paragraphs) and the READMEs in `tools/reference/`, `tools/recomp/`, `tools/worker/`, `src/`.
M3 was done on branch `ww02` (worktree `/srv/projects/WWHDRecomp/.worktrees/ww02`, based on
`ww-2`), pushed to the `worker` remote. M4 and G0 followed on the same branch.

## What the project is

A static **recompilation** of *The Legend of Zelda: The Wind Waker HD*. The target is the
USA v0 `cking.rpx` (sha256 in `orig/README.md`).
- The game's PowerPC code becomes C++ that runs on Cemu's OS libraries (HLE, scheduler, IOSU,
  audio, input).
- Graphics become our own Vulkan backend with shaders compiled ahead of time. That's the
  owner's choice: "native graphics from the start", not Cemu's Latte.

The scope is **single-screen, Pro Controller mode** (D17). The GamePad is reported absent, and
there's no DRC output. The owner may later build their own dual-screen feature (e.g. for AYN
Thor-like devices), but it isn't Nintendo's GamePad path, so don't design around it.

## Hard rules (from the owner and `CLAUDE.md`)

- **Never commit or emit game data.** That means no `.rpx`/`.rpl`/`.wua`, no extracted assets,
  no decompiler dumps and no generated recompiler output. Generated C++ and objects live only in
  `build/`, which is gitignored, as is `orig/`. It's a personal project on a legally owned copy.
- **Heavy work runs on the worker worker, never on the editing machine.** Heavy means Cemu, builds,
  Ghidra and big traces. A 272 MB trace once crashed the editing machine VM. the editing machine is for editing,
  git and light checks (the census and the generator run fine there).
- **Long jobs:** `tools/worker/job start NAME CMD…`, then `job wait NAME [MIN] [STALL_MIN]`, which
  returns 0 ok / 1 failed / 2 timeout / 3 stalled / 4 died.
  - Wait in bounded chunks (for example `job wait X 9 8`) and tell the owner what's running.
  - They explicitly asked for no silent multi-hour waits.
  - Scripts running under a job must print a heartbeat, or the stall detector fires.
- **Never kill by pattern** (`pkill -f`). A pattern kill once took out the wrong job. Use
  `job stop NAME`, or kill by PID/pidfile.
- Don't edit a script in place while a job runs it. `tools/worker/sync.sh` is fine: rsync
  replaces files by rename.
- **Commits:** `git -c user.name="flossbud" -c user.email="224492734+flossbud@users.noreply.github.com" commit …`, with
  the trailer `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Push with
  `git push worker <branch>`.
- **Every rename or retype needs evidence** (`config/US_v0/symbols.csv` has an evidence column).
  A function counts as "done" only once an external check passes (fixture or trace diff).
- **Talking to the owner:** they often read on a phone. Put choices as a numbered list at the end
  of the message so they can reply with a digit, and say plainly what's running and what's done.

## Infrastructure

- **Worker:** Docker `wwhd-worker` on the worker (a mini PC).
  - SSH alias `worker` (Tailscale), with key `~/.ssh/worker_ed25519`.
  - Limits: 24 GB RAM, no swap, 10 CPUs.
  - Volume `/wwhd` is a 250 GB sparse image. Paths: repo mirror `/wwhd/WWHDRecomp`, data
    `/wwhd/data` (ROM, traces, shots), logs `/wwhd/logs`.
  - `tools/worker/sync.sh` pushes the working tree. `tools/worker/w CMD` runs a command in the
    container.
- **Power cap:** the host has a 60 W RAPL cap (PL1 45 W / PL2 60 W). Its 90 W adapter latched off
  under full load before the cap. The host also keeps IP LAN_ADDR via a timer. Details are in
  the the owner's KB (`an incident note`). Don't undo the cap.
- **GPU:** Intel Intel iGPU through Mesa anv. Xvfb has no DRI3, so `run.sh` sets
  `MESA_VK_WSI_DEBUG=sw`. `REF_GPU=llvmpipe` forces software rendering.
- **Cemu source:**
  - the editing machine `~/opt/cemu-src`, branch `wwhd-reference`, for **editing only**; never build it
    on the editing machine.
  - The worker builds `/wwhd/opt/cemu-src` at pinned commit `c717fcab` plus
    `tools/reference/cemu-patches/0001–0011` (0011 is the execution seam, null by default).
  - To change a patch: commit on the editing machine branch, run
    `git format-patch -1 --start-number N -o tools/reference/cemu-patches/`, sync, then
    `tools/worker/job start cemu-rebuild tools/worker/setup-volume.sh cemu-rebuild` (incremental,
    about 5–10 min).

## What exists (all verified; numbers are from the worker)

**Reference harness** (`tools/reference/`, M0a done). A patched Cemu that's deterministic:
- virtual clock, synchronous IPC/GPU/ioctl, a single-core interpreter profile;
- an OS-call tracer, scripted Pro Controller input, frame-indexed screenshots, swkbd auto-answer.

The scripts:
- `run.sh` (env options documented in its header);
- `survey.sh ROUTE OUT LAST [STEP] [FIRST]` for contact sheets;
- `determinism.sh OUT FRAMES [ROUTE]`;
- `hle_trace.py summary|dump|diff`. The diff is vectorised: about 65M calls in 16 s. It takes
  `--ignore-core` and `--mask-cemu-area`.

`routes/title-to-game.txt` covers fresh boot → save dialog → title → controller select → file
select → name entry ("Link") → the legend intro → Aryll's dialogue → gameplay on Outset at
~f10450.

**Baseline traces** on the worker, route to f10800, **1,124,796,468 calls**, all identical:

| Trace | What |
|---|---|
| `/wwhd/data/traces/det-gpu/a.zst` | reference on the GPU |
| `/wwhd/data/traces/det-route/a.zst` | reference on llvmpipe |
| `/wwhd/data/traces/null-route.zst` | `wwhd-null` |

For 600 frames (59,531,239 calls): `det-wwhd/a.zst` and `det-null/a.zst`.

**Recompiler** (`tools/recomp/`, M1 and M2 done):
- **`ppc.py`:** the decoder.
- **`census.py`:** 2,351,474 instructions, 157 mnemonics, 0 undecodable. The game has no OE
  forms, no FP record forms and no absolute branches, and touches only the SPRs LR, CTR and
  UGQR2–5.
- **`emit.py`:** one instruction to C++, mirroring Cemu's interpreter, quirks included.
- **`fuzz/`:** runs emitted code against `PPCInterpreterSlim_executeInstruction`. All 154
  computing mnemonics match over about 1.8M random runs; NaN payloads are tolerated and counted.
  It links into Cemu_release's own link line via `-Wl,--wrap=main`.
- **`generate.py`:** the whole program, 39,720 functions: 39,705 from the list, the `.syscall`
  stub, and 14 synthesised GHS restore entries (D7). 0 errors.
- **`build.sh`:** compiles it on the worker in about 4 min, with 0 warnings; incremental since M3.
- Since M3 it also emits purity (12,968 pure functions), code hashes, call edges, import sites and
  a store census (`runtime/recomp_tables.h`). Two M2 bugs were fixed: all 294 jump tables are `b`
  runs (the switches never matched), and `_iob+0x10` was emitted as `environ`.

**Runtime** (`src/runtime/`, M3 and M4 done). Linked into `wwhd-null` with the generated program:
- **`WWHD_NATIVE=on` runs the recompiled program.** The whole-route trace equals the reference's
  (1,124,796,468 calls) in 359 s, with 0 game instructions interpreted. Guest time is exact per
  instruction (`RT_TICK`/`rt_yield`, design D6 as built), and Cemu's boot patches are in the
  generated code (`config/US_v0/code_patches.csv`, D10).
- Cemu patch 0011's hook replaces the interpreter loop; by default it interprets exactly.
- At boot: function table (D5), code hashes vs guest memory (Cemu patches `f_027F9994` and
  `f_028137E0`, D10), imports bound from guest memory (397 functions, 10 data; 0 differences from
  Cemu's name tables).
- All `rt_*` exist. `rt_import`/`rt_call_ctr`/`rt_jump_ctr` are bound and checked but only run once
  non-pure code goes native (M4).
- `WWHD_NATIVE=diff`: diff mode (D8.2). Over the whole route: 3,875,261 sampled pure calls checked,
  all equal to the interpreter; 4,616 functions, all clean; trace identical to `null-route.zst`.
  Since M4 it also compares cycles: clean over the whole route as well.
- `WWHD_GPU_STATS=path` (null GPU) and `tools/reference/g0_gx2.py`: the G0 measurements (D15).
- `WWHD_GPU_DUMP=dir` (null GPU) and `tools/shaders/`: the G1 corpus, 30,011 programs, all
  translated to valid SPIR-V (D14).
- `tools/reference/route.sh OUT FRAMES ROUTE BASELINE`: one run plus a trace comparison.
- `WWHD_RENDER=vk` (`src/gpu/vk`): the G2 renderer; `survey.sh` captures and
  `compare_frames.py` compares with `/wwhd/data/g2/ref` (the reference's captures, f30-f600 every 30).

**Runtime** (`src/`, M0b done). `src/build.sh` builds two binaries against the worker's Cemu:
- **`build/wwhd/wwhd`:** our frontend (`src/frontend`, Cemu's `WindowSystem` without wxWidgets,
  an Xlib window) with Latte on Vulkan. Its frames are byte-identical to Cemu_release's.
- **`build/wwhd/wwhd-null`:** headless, with no Latte. `src/gpu/null_gpu.cpp` is a command
  processor that keeps the register file and does every guest-visible effect but draws nothing.
  **Its trace is identical to the reference over the whole route, in 12 min.** It's the fast
  harness for CPU work.
  - It must be linked with its archive members in `wwhd`'s order (`src/link_order.py`).
    Otherwise Cemu's `SysAllocator` slots shift and addresses in 0x0E000000+ differ.
  - D12 was revised: Cemu's gx2 and TCL stay as the exact front half, and only Latte is
    replaced.

## Next steps, in order

M3, M4, G0 and G1 are done, M4 and G0 on the scripted route (design doc status paragraphs). `wwhd-null` with
`WWHD_NATIVE=on` runs the recompiled program, and its whole-route trace equals the reference's.
G2 renders the title screen; closing it is step 1.

### 1. G2: close the title screen (design D13, D16.3)

The renderer exists (design doc "G2 status", `src/README.md`): `WWHD_RENDER=vk` in `wwhd-null`
renders the title screen on lavapipe, visually the same as the reference. Frames f150-f390 are at
36-53 dB; the title frames f420-f600 are at 24.6-27.4 dB. The trace with rendering on equals
`det-null/a.zst`. Left:
- **The bloom halo** on the title is smaller than the reference's. The chain (one R11G11B10 texture,
  480x270 with 4 mips at `f4000800`, levels drawn as separate targets) is assembled from the
  per-level surfaces, and every level has plausible content (`WWHD_RENDER_DUMP=480`). Suspects:
  how Cemu's texture cache relates the 480x270 texture to the 480x272 target and to the padded
  level targets (256x256, 128x128, 64x64), and the blur's ping-pong targets (256x144). A dump of
  the reference's targets at the same frame (a Cemu patch) would settle it.
- **A numeric tolerance:** D16.3 names SSIM; `compare_frames.py` reports PSNR. References against
  each other are at 73-75 dB.
- Not on the route yet, so untested: GPU-side `GX2CopySurface` (`IT_HLE_COPY_SURFACE_NEW`, only
  reported), readback of a rendered surface into a linear-special destination, 3D textures,
  depth-stencil textures loaded from memory (zeroed, as Cemu does).
- The G1 translator now sets the pixel-shader input table like the renderer. Re-translated since:
  the 287 variants (574 modules) and the whole corpus (30,011), all valid.

### 2. Extend the route, then rerun G0 and the native check

The route stops on Outset. D15 also wants sailing, a dungeon room, the menus and the Pictograph
Box. Extend `routes/title-to-game.txt` (with `survey.sh` for contact sheets), record new baselines
with the reference, and rerun:
- `route.sh` in native mode against the new baseline;
- `g0_gx2.py` on the new trace, and `WWHD_GPU_STATS` in the same native run.

### 3. Own CMake build for `src/` and an SDL3 window

- `src/build.sh` currently borrows Cemu's link line (wx libraries still listed, but harmless).
- Cemu's vcpkg SDL3 is built with `default-features: false`, so it has **no video backends**,
  which is why the window is plain Xlib.
- Needed before interactive play, Wayland and Windows: a proper CMake project building Cemu's
  sources without Latte (design D11), SDL3 with video, and keyboard/controller input wired to
  `WindowSystem` (the frontend has no keyboard input yet; only scripted input works).

### 4. Later

- G3 (every scene on the route), M5 (playable on a GPU machine), M6 (60 fps).
- Native speed, when it matters: per-block counting (D6), host-local registers (D2). Keep the
  whole-route trace check for every step. The shipped real-clock build may count coarsely behind a
  flag (D6).
- The recomp needs its own name-entry keyboard; the reference uses `CEMU_SWKBD_AUTO`.
- The GamePad view (an 864×480 pass, about 7 draws per frame) can be skipped with an override now
  that the renderer exists.

## Known facts and gotchas worth not rediscovering

- **Cemu's interpreter quirks the emitter mirrors:**
  - singles round through `float`;
  - `fmuls`/`fmadds` truncate frC to 25 bits (`roundTo25BitAccuracy`);
  - `ps_mul`/`ps_madd` flush denormals;
  - `divw` by 0 gives 0 or -1.
  - Also, `fcmpo` decodes as `fcmpu`, and `mtfsb0`, `mtfsfi` and `ps_cmpo1` are unimplemented;
    the game uses none of these.
  - Build generated code with `-ffp-contract=off` (Cemu targets baseline x86-64, so it never
    fuses).
  - Never write `-(sint32)x`: it's UB for INT_MIN and clang exploits it. Use `0u - x`.
- **Relocations:**
  - all internal relocations are already applied in the file;
  - calls to imports are `REL24` into `.fimport_*`;
  - 58 immediates point at `.dimport_*` data imports;
  - two `bl` go to weak address 0.
  - `functions.csv` lists only function imports, so `generate.py` reads imports (426) from the
    RPX symbol table.
- **GHS save/restore helpers** at 0x028F5EE0–0x028F626C: epilogues branch into their middles,
  and `generate.py` synthesises those entries (D7).
- **Reference determinism took patches 0001–0006** (`tools/reference/README.md`, "What it took").
  Screenshots of two runs can differ by a few hundred llvmpipe edge pixels, but the traces don't.
- **Recompiler facts found in M3:** jump tables are `b` runs (switch on CTR's slot address);
  relocations must be keyed by symbol (addends exist: `_iob+0x10`); the GHS restore-and-exit helpers
  return to their caller's caller; `spr.XER`'s CA/SO/OV copies go stale after a context switch
  (compare `PPCInterpreter_getXER`); Cemu's `tw` with TO=0 is its debugger's breakpoint.
- **Size and speed:** a full-route trace is about 665 MB compressed. The GPU reference route
  takes about 25 min; `wwhd-null` about 12 min.
- **Cemu's GX2 writes Cemu-only `IT_HLE_*` packets into display lists,** so their sizes differ
  from real GX2. That's why its gx2 stays (D12).

## Unfinished odds and ends

- An upstream report of nWiiURecomp's missing `fdivs` validator (`xo5==18`) was never posted.
- `symbols.csv` has only 110 names (from the TWW randomizer's linker map). Phase 2 naming is
  needed for M6.
