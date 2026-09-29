# Handoff: WWHD recomp, state as of 2026-09-29

Read this first, then `CLAUDE.md`, `docs/recompiler-design.md` (decisions D1–D17, milestones,
status paragraphs) and the READMEs in `tools/reference/`, `tools/recomp/`, `tools/worker/`, `src/`.
Branch `ww-2` in the worktree `/srv/projects/WWHDRecomp/.worktrees/ww-2`, pushed to the
`worker` remote; the last commit when this was written was `c5f0062`.

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
  `git push worker ww-2`.
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
    `tools/reference/cemu-patches/0001–0010`.
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
- **`build.sh`:** compiles it on the worker in about 4 min, with 0 warnings. The output uses
  `rt_import`, `rt_import_data`, `rt_call_ctr`, `rt_jump_ctr` and `rt_bad_branch`, which **don't
  exist yet** (they come with M3).

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

### 1. M3: run recompiled code, starting with pure functions under diff mode

This is the first time generated code executes. Keep every sub-step checkable against the
baseline traces.

1. **Execution seam, as a new Cemu patch (0011).**
   - Add a hook pointer (null by default, so the reference is unchanged) at the two places D1
     names: the run loop in `__OSFiberThreadEntry` (`coreinit_Thread.cpp`, around line
     1425–1431, `while (--remainingCycles >= 0) PPCInterpreterSlim_executeInstruction`) and
     `PPCCore_executeCallbackInternal` (`PPCScheduler.cpp`).
   - Our runtime sets the hook. When the hook finds no native function, it must fall back to
     exactly the interpreter.
   - Re-run `determinism.sh` with the hook installed but **everything still interpreted**. The
     trace must equal the baseline.
2. **The `rt_*` runtime** (a new `src/runtime/`):
   - **`rt_import(ctx, id)`:** bind each `g_imports[]` entry (lib, name) to Cemu's HLE handler
     the way Cemu's loader does (`rpl_mapHLEImport`, `Cafe/OS/RPL/rpl.cpp` ~746;
     `PPCInterpreter_getHLECall`). Afterwards, check `instructionPointer == LR` for handlers that
     redirect (D4: MEM allocator forwarding).
   - **Record the call in the trace:** the reference's tracer records at the HLE trampoline, so
     native import calls must also call `HLETrace::record` and charge `kVirtualHLECallCycles`
     (300). Otherwise the traces aren't comparable.
   - **`rt_import_data(id)`:** the address Cemu's loader resolved for that data import.
   - **`rt_call_ctr` / `rt_jump_ctr`:** the function table (D5, a flat array over .text), then
     the trampoline area (the HLE path), otherwise the interpreter.
   - **`rt_bad_branch`:** a loud error.
3. **Link the generated objects into `wwhd-null`.** The link is large; keep LTO off for
   generated code. A good initial test is a special mode that runs one chosen native function.
4. **Guest time is the biggest design issue.**
   - **The problem:** under the virtual clock, guest time **is** the instruction count
     (`remainingCycles` is decremented per instruction; timeslices and vsync hang off it). Native
     code that charges different counts, or yields at different points, changes thread
     interleaving, so the trace stops matching the reference. D6 predates the virtual clock and
     says coarse accounting is fine; that no longer holds if we want trace equality.
   - **For M3 (diff mode) there's a clean way out:** run native with stores journaled, rewind,
     run the interpreter over the same call (which charges the cycles exactly as the reference
     does), and compare registers and memory writes. The run then stays trace-identical to the
     baseline while every sampled call gets checked.
   - **For M4 (native for real), decide deliberately.** Either charge exact per-instruction counts
     (per basic block, with a way to stop mid-block at quantum expiry), or relax the check to
     per-thread call-sequence equality. Write the decision into the design doc.
5. **Mark pure functions from the call graph** (they reach no import and no indirect call, D8.2),
   and turn diff mode on for them along the route with `wwhd-null`. Done means M3's criterion: a
   clean sampled diff over the scripted route.
6. The yield budget (D6), `GamePatch` (D10), and "everything native" are **M4**.

### 2. G0: scope the graphics backend (measurement only; can run beside M3)

- Trace every GX2 call and argument in single-screen Pro Controller mode along the route
  (`REF_LOGFLAG` or the HLE tracer filtered with `CEMU_HLE_TRACE_FILTER=gx2.`), plus the PM4
  register state the null GPU sees.
- Answer D13's open features: geometry shaders, MSAA/`GX2ExpandAAColorBuffer`, HiZ,
  stream-out, whether a GamePad view is still rendered, and the shader variant count (open
  question 5).
- `GX2CopySurface` and occlusion queries need attention:
  - the null GPU skips the CPU-copy path and answers no queries;
  - WWHD didn't use queries up to gameplay, and the traces still match;
  - but a real backend must handle whatever G0 finds.
- The Vulkan backend (D13) will be a renderer that reads the null GPU's register file (see the
  revised D12). G1 is the AOT shader corpus.

### 3. Own CMake build for `src/` and an SDL3 window

- `src/build.sh` currently borrows Cemu's link line (wx libraries still listed, but harmless).
- Cemu's vcpkg SDL3 is built with `default-features: false`, so it has **no video backends**,
  which is why the window is plain Xlib.
- Needed before interactive play, Wayland and Windows: a proper CMake project building Cemu's
  sources without Latte (design D11), SDL3 with video, and keyboard/controller input wired to
  `WindowSystem` (the frontend has no keyboard input yet; only scripted input works).

### 4. Later

- M4 (everything native), G1–G3, M5 (playable on a GPU machine), M6 (60 fps).
- The recomp needs its own name-entry keyboard; the reference uses `CEMU_SWKBD_AUTO`.
- Extend the route beyond the lookout when the CPU track needs more coverage.

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
- **Size and speed:** a full-route trace is about 665 MB compressed. The GPU reference route
  takes about 25 min; `wwhd-null` about 12 min.
- **Cemu's GX2 writes Cemu-only `IT_HLE_*` packets into display lists,** so their sizes differ
  from real GX2. That's why its gx2 stays (D12).

## Unfinished odds and ends

- An upstream report of nWiiURecomp's missing `fdivs` validator (`xo5==18`) was never posted.
- `symbols.csv` has only 110 names (from the TWW randomizer's linker map). Phase 2 naming is
  needed for M6.
