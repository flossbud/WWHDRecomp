# Handoff: WWHD recomp, state as of 2026-09-30 (night)

Read this first, then `CLAUDE.md`, `docs/recompiler-design.md` (decisions D1–D20, milestones,
status paragraphs) and the READMEs in `src/`, `tools/reference/`, `tools/recomp/`, `tools/worker/`.
Work is on branch `ww-3` (worktree `/srv/projects/WWHDRecomp/.worktrees/ww-3`, based on `ww02`),
pushed to the `worker` remote. `ww02` holds the work before it.

## What the project is

A static **recompilation** of *The Legend of Zelda: The Wind Waker HD*. The target is the
USA v0 `cking.rpx` (sha256 in `orig/README.md`).
- The game's PowerPC code becomes C++ (`tools/recomp`), linked into our runtime (`src/`).
- **The shipped game will not depend on Cemu** (design D18). Cemu stays the *reference*: a patched,
  deterministic Cemu produces the OS-call traces, GPU command streams and sound every change is
  checked against. Its OS libraries are being replaced piece by piece (our OS layer and forks,
  below), least coupled first.
- Graphics are our own Vulkan renderer (`src/gpu/vk`), not Cemu's Latte: "native graphics from the
  start", the owner's choice.

The scope is **single-screen, Pro Controller mode** (D17). The GamePad is reported absent, and
there's no DRC output. The owner may later build their own dual-screen feature (e.g. for AYN
Thor-like devices), but it isn't Nintendo's GamePad path, so don't design around it.

**Keep a future Android release in mind** (the owner asked for this): no x86-only pieces without a
portable fallback, Vulkan features Android drivers have, work that can move to build time does, and
startup time and CPU use matter (phones throttle when hot). Design D19 and D20 have Android notes.

## Hard rules (from the owner and `CLAUDE.md`)

- **Never commit or emit game data.** That means no `.rpx`/`.rpl`/`.wua`, no extracted assets,
  no decompiler dumps, no generated recompiler output, no shader caches, captures or saves.
  Generated C++ and objects live only in `build/`, which is gitignored, as is `orig/`. It's a
  personal project on a legally owned copy. Core dumps contain game memory: delete them.
- **Heavy work runs on the worker worker, never on the editing machine.** Heavy means Cemu, builds,
  Ghidra and big traces. A 272 MB trace once crashed the editing machine VM. the editing machine is for editing,
  git and light checks (the census and the generator run fine there). Real-time play and real-time
  measurements go to the owner's desktop (below).
- **Long jobs:** `tools/worker/job start NAME CMD…`, then `job wait NAME [MIN] [STALL_MIN]`, which
  returns 0 ok / 1 failed / 2 timeout / 3 stalled / 4 died; `job tail NAME` shows the log.
  - A tool call can wait at most 10 minutes: wait in chunks (`job wait X 9`) and tell the owner
    what's running between chunks. They explicitly asked for no silent multi-hour waits.
  - Scripts running under a job must print a heartbeat, or the stall detector fires.
- **Never kill by pattern** (`pkill -f`). A pattern kill once took out the wrong job. Use
  `job stop NAME`, or kill by PID/pidfile.
- Don't edit a script in place while a job runs it. `tools/worker/sync.sh` is fine: rsync
  replaces files by rename.
- **Commits:** `git -c user.name="flossbud" -c user.email="224492734+flossbud@users.noreply.github.com" commit …`, with
  the trailer `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Push with
  `git push worker ww-3`. Commit when a step is done and its checks pass.
- **Every rename or retype needs evidence** (`config/US_v0/symbols.csv` has an evidence column).
  A function counts as "done" only once an external check passes (fixture or trace diff).
- **Talking to the owner:** they often read on a phone. Put choices as a numbered list at the end
  of the message so they can reply with a digit, and recommend one. Say plainly what's running and
  what's done, and give a short progress line during long stretches of work.
- **The owner's desktop is theirs:** test there headless (no window, no sound) unless they asked to
  see or hear something, and say so before opening a window on it.

## Infrastructure

- **Worker:** Docker `wwhd-worker` on the worker (a mini PC, 6-core CPU).
  - SSH alias `worker` (Tailscale), with key `~/.ssh/worker_ed25519`.
  - Limits: 24 GB RAM, no swap, 10 CPUs.
  - Volume `/wwhd` is a 250 GB sparse image. Paths: repo mirror `/wwhd/WWHDRecomp`, data
    `/wwhd/data` (ROM, traces, saves, captures), logs `/wwhd/logs`.
  - `tools/worker/sync.sh` pushes the working tree. `tools/worker/w CMD` runs a command in the
    container (stdin passes through: `tools/worker/w python3 - args < script.py`).
- **Power cap:** the host has a 60 W RAPL cap (PL1 45 W / PL2 60 W). Its 90 W adapter latched off
  under full load before the cap. The host also keeps IP LAN_ADDR via a timer. Details are in
  the the owner's KB (`an incident note`). Don't undo the cap.
  It's also why real-time runs on the worker are slow: time real time on the desktop.
- **GPU (worker):** Intel Intel iGPU through Mesa anv. Xvfb has no DRI3, so `run.sh` sets
  `MESA_VK_WSI_DEBUG=sw`. `REF_GPU=llvmpipe` forces software rendering (lavapipe), which every
  frame comparison uses.
- **Cemu source:**
  - the editing machine `~/opt/cemu-src`, branch `wwhd-reference`, for **editing only**; never build it
    on the editing machine.
  - The worker builds `/wwhd/opt/cemu-src` at pinned commit `c717fcab` plus
    `tools/reference/cemu-patches/0001–0013` (0011 is the execution seam).
  - To change a patch: commit on the editing machine branch, run
    `git format-patch -1 --start-number N -o tools/reference/cemu-patches/`, sync, then
    `tools/worker/job start cemu-rebuild tools/worker/setup-volume.sh cemu-rebuild` (incremental,
    about 5–10 min). Our forks (`src/forks.txt`) are no longer reached by cemu-patches.
  - Ghidra and its project (`ghidra/projects/`, disposable) are on the worker:
    `tools/ghidra/headless.sh`, `rebuild.sh`.
- **The owner's desktop, `desktop`** (on the tailnet): Fedora 44, desktop CPU (24 threads),
  AMD GPU on RADV, GNOME on Wayland, three monitors, speakers.
  - SSH: `ssh owner@DESKTOP_ADDR` (the default key works with `BatchMode=yes`). Use the IP: the
    name `desktop` resolves to a public address through a search domain.
  - It suspends when idle and then drops off the tailnet (`tailscale status` shows "offline").
    That is not a crash.
  - `~/wwhd-play`: **the owner's** copy (program, Cemu's data files, the game, the test save, and in
    `portable/` their emulated NAND, saves and shader cache). Don't reset their saves or cache.
  - `~/wwhd-test`: the agents' copy for headless tests (its `game/wwhd.wua` links to wwhd-play's).
  - `tools/play/deploy.sh owner@DESKTOP_ADDR [DIR]` (from the editing machine, after `src/build.sh`)
    streams the build there. It replaces the binary by rename (a running game is untouched) and
    rewrites `play.sh`, `portable/settings.xml` and the game profile.
  - Headless test: `cd ~/wwhd-test && WWHD_WINDOW=0 WWHD_SAVE=saves/wwhd_100
    CEMU_INPUT_SCRIPT=$PWD/routes/continue-100.txt WWHD_EXIT_FRAME=1800 ./play.sh`. With
    `WWHD_WINDOW=0` the sound goes to `/dev/null` (`WWHD_AUDIO_HASH`). Before that, a test played
    through the owner's speakers. Results: the `wwhd real time:` lines in `portable/log.txt`;
    `WWHD_PROFILE=/tmp/p.txt` records a profile (Known facts says how to read it).
  - Launching the window for the owner (only when they ask): `systemd-run --user --unit=wwhd-play
    --collect -p WorkingDirectory=$HOME/wwhd-play -p StandardOutput=file:$HOME/wwhd-play/play.log
    -p StandardError=file:$HOME/wwhd-play/play.log $HOME/wwhd-play/play.sh` (the user manager has
    the Wayland environment). The keys are in `tools/play/play.sh`'s header.

## What exists (all verified)

**Reference harness** (`tools/reference/`, M0a done). A patched Cemu that's deterministic:
- virtual clock, synchronous IPC/GPU/ioctl, a single-core interpreter profile;
- an OS-call tracer, scripted Pro Controller input, frame-indexed screenshots, swkbd auto-answer.

The scripts:
- `run.sh` (env options documented in its header; it discards the program's stdout);
- `route.sh OUT FRAMES ROUTE BASELINE`: one run plus a trace comparison;
- `stream_check.sh save|route NAME`: trace, GPU command stream and sound against the baselines;
- `timing.sh save|route OUT`: speed without the trace, with the profiler;
- `survey.sh ROUTE OUT LAST [STEP] [FIRST]` for contact sheets; `compare_frames.py` for captures;
- `shader_cache_check.sh OUT`: the shader cache changes nothing on screen (D20);
- `determinism.sh OUT FRAMES [ROUTE]`; `hle_trace.py summary|dump|diff` (about 65M calls in
  16 s, `--ignore-core`, `--mask-cemu-area`).

**Routes** (`tools/reference/routes/`; input is keyed on the swap count, so routes also play in
real time):
- `title-to-game.txt`: fresh boot → save dialog → title → controller select → file select → name
  entry → the legend intro → Aryll → gameplay on Outset at about f10450;
- `continue-100.txt`: from the owner's 100% save, gameplay on the Outset dock at f870;
- `tour-100.txt`: continue-100, then walk the dock and open the pause menu.

**Baselines** on the worker:

| Baseline | What |
|---|---|
| `/wwhd/data/traces/null-route.zst`, `det-gpu/a.zst`, `det-route/a.zst` | whole route to f10800, **1,124,796,468 calls**, all identical |
| `/wwhd/data/traces/save-det/{a,b}.zst` | save route to f1800, **172,954,163 calls** |
| `/wwhd/data/gx2/{save,route}-cemu.txt` | GPU command streams: 45,955,744 and 143,098,119 packets |
| `/wwhd/data/gx2/{save,route}-audio-cemu.txt` | sound: 5,180 and 30,219 blocks |
| `/wwhd/data/g2/ref`, `/wwhd/data/g3/save-ref` | reference captures (title route f30–f600 every 30; save route every 60), with cemu-patches/0013 |

**The 100% save** (three quest logs; log 1: full Triforce, 3 pearls, 20 hearts, Normal Mode, saved
on Outset) is at `/wwhd/data/saves/wwhd_100` on the worker and `~/wwhd-play/saves/wwhd_100` on the
desktop. `REF_SAVE=dir` (run.sh) or `WWHD_SAVE=dir` (play.sh) installs it. It is game data.

**Recompiler** (`tools/recomp/`, M1 and M2 done):
- `ppc.py`, the decoder.
- `census.py`: 2,351,474 instructions, 157 mnemonics, 0 undecodable.
- `emit.py`: one instruction to C++, mirroring Cemu's interpreter, quirks included.
- `fuzz/`: emitted code against Cemu's interpreter. Run `tools/recomp/fuzz/build.sh`, then
  `build/fuzz/fuzz ITER MNEMONIC`; record forms are fuzzed under the base mnemonic.
- `generate.py`: 39,720 functions, 0 errors, with exact per-basic-block cycle counting (D6 as
  built). It also emits purity (12,968 pure functions), code hashes, call edges, import sites and
  a store census.
- `build.sh`: compiles on the worker in about 11 min, longer than one tool call.

**Runtime** (`src/`; `src/build.sh` builds on the worker in about 5 min):
- **`build/wwhd/wwhd-null` is the product.** It holds the recompiled program
  (`WWHD_NATIVE=on`), our OS layer, the null GPU and, with `WWHD_RENDER=vk`, our renderer.
  - The null GPU (`src/gpu/null_gpu.cpp`) is a command processor that keeps the register file and
    does every guest-visible effect.
  - `build/wwhd/wwhd` is Cemu's Latte with our frontend, kept for comparison.
- **Link order matters.** Cemu's archives are linked in `wwhd`'s member order
  (`src/link_order.py`), or Cemu's `SysAllocator` slots shift and guest addresses in 0x0E000000+
  differ. Forks replace Cemu's objects at the same position. Archives that lose objects are
  linked as `lib<name>_wwhd.a`.
- **`WWHD_NATIVE=diff`** is diff mode (D8.2): pure calls and cycles checked against the
  interpreter. Latest on the save route: 1,118,003 checked calls, 0 mismatches.
- **Our OS layer** (`src/os`, src/README):
  - 255 imports are ours: coreinit 31, gx2 179, nn_ac 2, nn_act 2, padscore 6, vpad 4,
    erreula 15, swkbd 16.
  - **25 Cemu source files are forked** (`src/forks.txt`): snd_core; the scheduler and its
    threads, queues, alarms and sync; gx2's core; TCL; proc_ui; the HLE dispatch; the timer;
    fibers.
  - Forks started as Cemu's code and differ where the design doc says.
  - `WWHD_OS=cemu` turns our imports off.
- **The platform shell** (`WWHD_WINDOW=1`): an SDL3 window presented by our renderer, keyboard and
  gamepad as the Pro Controller, TV sound on SDL3. The system keyboard and error dialogs are drawn
  by the frontend (`frontend/overlay.cpp`).
- **Real time on one host thread** (D19 step 2), when `CEMU_VIRTUAL_CLOCK` is unset:
  - guest time is `steady_clock`;
  - the idle scheduler sleeps, and the null GPU sleeps until the host-timed vsync;
  - every 10 s `portable/log.txt` gets a line: fps, frame-time median, 99th percentile and worst,
    how busy the scheduler thread was, how often the task loop slept, and first sights of shaders
    and pipelines;
  - the fiber context switch is our own (x86-64 assembly, ucontext elsewhere).
- **Overrides** (D9 as built): `config/US_v0/overrides.txt` lists generated functions we replace;
  `generate.py` emits their bodies as `orig_f_X`, `src/overrides/` defines `f_X`, and the linker
  catches a missing or unlisted override. One so far: the game's task loop.
- **Real-time fast paths** (D19): overrides that only run in real time on one host thread
  (`wwhd::rt::FastPaths`; `WWHD_FAST_PATHS=0` turns them off). The task loop (`f_0275FFCC`,
  `src/overrides/task_loop.cpp`) sleeps through the rounds where the game's ticking task only posts
  itself its tick again, proven by the runtime's *quiet watch* (the store journal notes any live
  store). The scheduler thread went from 100% busy to 8–25% on the save route.
- **The renderer** (`src/gpu/vk`, D13; G2 done on the title route): Cemu's shader decompiler to
  GLSL, glslang to SPIR-V, Vulkan 1.3 with dynamic rendering.
- **The shader cache** (D20, `src/gpu/vk/shader_cache.cpp`), in `portable/shaderCache/wwhd`:
  - it keeps shaders (SPIR-V plus the decompiler facts the draws read), pipeline recipes and the
    driver's `VkPipelineCache`;
  - everything cached is built before the title launches, with a "Preparing shaders" screen when
    that takes over 0.3 s;
  - `WWHD_SHADER_THREADS=n` sets how many threads build pipelines.
- **Playing on a desktop:** `tools/play/deploy.sh` and `tools/play/play.sh` (above).

**Speed** (`timing.sh`, virtual clock, no trace, from launch):

| | Save route (1800 frames) | Whole route (10800) |
|---|---|---|
| Before the quick wins | 44 s | — |
| Now | 29.6 s, 2.03x real time | 117.7 s, 3.06x |

- **Profile of the CPU thread** (save route, virtual clock): guest code 81%, message queues 8%,
  helpers 3%, HLE dispatch 2.5%, gx2 1.5%, thread switches 0.9%.
- **Ruled out:** keeping guest registers in host locals was measured and rejected (the D2 note).
- **In real time on the desktop** (save route, headless): a steady 30 fps and a 99th-percentile
  frame time of 34–35 ms once loaded. The scheduler thread is busy 8–25% (100% before the task
  loop's fast path), and the whole process uses 23 s of CPU per minute of play (69 s before). What
  is left is the game's work: audio mixing and decompression in other task threads, and the frame.

**Which checks for which change:**

| You changed | Run |
|---|---|
| anything the guest can see (OS layer, scheduler, forks, runtime, generated code) | `stream_check.sh save NAME`, then `route NAME`: trace, command stream and sound identical |
| generated code or its runtime helpers | the fuzzer for touched mnemonics; diff mode (below): MISMATCH 0 |
| speed | `timing.sh save OUT` (and `route`), then `python3 tools/profile_report.py OUT/profile.txt` |
| the renderer or the shader cache | `shader_cache_check.sh`; `survey.sh` + `compare_frames.py` against the reference captures (threshold 60 dB), or against your own "before" captures (byte-identical when nothing on screen should change) |
| real-time behaviour | headless on the desktop (above): the `wwhd real time:` lines, and a profile |

Diff mode on the save route:
`tools/worker/job start diff env CEMU_BIN=/wwhd/WWHDRecomp/build/wwhd/wwhd-null WWHD_NATIVE=diff
WWHD_RT_LOG=/wwhd/data/traces/diff/rt.log REF_SAVE=/wwhd/data/saves/wwhd_100
tools/reference/route.sh /wwhd/data/traces/diff 1800 tools/reference/routes/continue-100.txt
/wwhd/data/traces/save-det/a.zst`, then `grep "diff (final)" /wwhd/data/traces/diff/rt.log`.

## Next steps, in order (the owner chose 1, then 2, then onwards)

### 1. D9 overrides, then let the game's task switcher sleep in real time: done (WW-3)

- **Overrides** exist (D9 "As built"): `config/US_v0/overrides.txt`, `orig_f_X`, `src/overrides/`.
- **The spin** was the game's task loop, `f_0275FFCC` (`task_MessageLoop`; the task library's names
  and evidence are in `symbols.csv`). One task (thread 1603f4a8) posts itself message 5 after every
  message, and a 5 does nothing while it's idle: 34,938 send/receive pairs in frame 1500, nothing
  else. Its handler's likely path is `f_0203DEEC` (a switch on messages 1–5, each reposting 5) and
  `f_0203DDC0` (state at +56; state 1 looks at a time stamp at +120), not yet named or checked.
- **The fast path** (D19 "The task loop sleeps"): in real time, a handler call that stored nothing
  live, made one OS call and only reposted its message makes the next round of that message wait
  (a sender wakes it, else 1 ms), so the host thread sleeps. The virtual clock runs `orig_f_0275FFCC`.
- **Checked:** both routes' `stream_check` identical, diff mode clean (see D19); on the desktop,
  scheduler thread 8–25% busy (was 100%), 30 fps, 99th 34–35 ms, CPU 23 s/min (was 69).
- **Still open:** the owner listens to the sound in a window (below, "Launching the window").
- **Ideas, not needed now:** a longer wait than 1 ms (the next vsync) would cut the ~900 wake-ups a
  second, at the cost of noticing memory-only changes later; D19's three-host-thread mode would need
  the quiet watch per host thread.

### 2. Fewer pipelines: more dynamic state (D20 "Next" b)

**Today** (`PipelineDesc` in `src/gpu/vk/draw.cpp`):
- Baked into every pipeline: vertex input, topology, primitive restart, rasterizer discard,
  depth-bias enable, cull mode, front face, depth clip, per-attachment blend and write masks,
  logic op, depth test/write/compare, stencil test/ops, **stencil reference and masks**, and
  attachment formats.
- Already dynamic: viewport, scissor, blend constants, depth-bias values.
- The save route builds 432 pipelines from 633 shaders.

**The plan:**
1. Make stencil reference, compare mask and write mask dynamic state (core Vulkan 1.0). Use
   `vkCmdSetStencilReference`, `vkCmdSetStencilCompareMask` and `vkCmdSetStencilWriteMask`,
   added to the function table in `vk.h`, set per draw for front and back.
2. Then extended dynamic state 1 and 2, which are core in Vulkan 1.3 (already required by the
   renderer): cull mode, front face, topology within its class, depth test/write/compare, stencil
   test and ops, rasterizer discard, depth-bias enable, primitive restart.
3. Extended dynamic state 3 (blend, write masks, logic op, depth clip) only where the device has
   it, with a fallback: Android support is patchy.
4. `PipelineDesc` changes meaning, so bump `kVersion` in `shader_cache.cpp`.

**Done when:**
- pipeline counts on the save and title routes are down (report before and after);
- captures are byte-identical to captures made before the change (same settings, lavapipe);
- `shader_cache_check.sh` passes;
- D20 says what's dynamic now.

An Android note: requiring Vulkan 1.3 (for dynamic rendering) already excludes many older phones.
That's a separate decision for the owner.

### 3. Then, roughly in this order (ask the owner)

- **The first playthrough without hitches** (D20 "Next" a). Gather shader keys and pipeline
  recipes from playthroughs and ship the list with the port: it holds hashes and register values,
  no game content. The player's machine makes the shaders from its own game files (the G1
  extractor, D14, finds the programs), so the progress screen appears on the first start instead
  of hitches.
- **G3: render the whole route** (D13, D16.3).
  - Where it stands: G2 is done to f600 of the title route (within 60 dB of the reference, most
    within one level), and the save route's dock frames are at 55–61 dB.
  - Method: capture the same frames on both sides (`survey.sh`), then
    `compare_frames.py --threshold 60`.
  - For a difference: compare surfaces at that swap (`CEMU_TEX_DUMP_FRAME` on the reference,
    `WWHD_RENDER_DUMP` on ours, `compare_dumps.py`), then `WWHD_RENDER_TRACE` with a pixel to find
    the draw. `WWHD_RENDER_SHADERS=dir` writes each shader's GLSL.
  - Untested so far:
    - GPU-side `GX2CopySurface` (only reported);
    - readback into linear-special destinations;
    - 3D textures and cube-map targets;
    - depth-stencil textures loaded from memory;
    - one address and format at different sizes (the bloom blur's ping-pong targets share one
      surface).
- **Extend the routes**: sailing, a dungeon room, the menus and the Pictograph Box. Start from the
  100% save and warp with the Ballad of Gales. Record new baselines with the reference, then run
  native, G0 and timing on them. D19 step 4 uses heavier routes to decide between one host thread
  and three.
- **Our own CMake build** for `src/`. Today `src/build.sh` borrows Cemu's link line. It's needed
  before Windows, macOS or Android.
- **The rest of D18:**
  - the file system, with `nn_save`;
  - the loader and memory map, giving the guest OS objects in Cemu's memory (the SysAllocators)
    their own home;
  - gx2's core rewritten;
  - proc_ui.
- **Later:**
  - M5, playable on a GPU machine (the desktop is one now);
  - M6, 60 fps, via overrides written against Phase 2 names;
  - an arm64 context switch for Android (D19);
  - the GamePad view skipped with an override.

## Known facts and gotchas worth not rediscovering

- **Cemu's interpreter quirks the emitter mirrors:**
  - singles round through `float`;
  - `fmuls`/`fmadds` truncate frC to 25 bits;
  - `ps_mul`/`ps_madd` flush denormals;
  - `divw` by 0 gives 0 or -1;
  - `fcmpo` decodes as `fcmpu`;
  - `mtfsb0`, `mtfsfi` and `ps_cmpo1` are unimplemented, and the game doesn't use them.
  - Build generated code with `-ffp-contract=off`. Never write `-(sint32)x`; use `0u - x`.
- **Relocations:**
  - internal ones are already applied in the file;
  - import calls are `REL24` into `.fimport_*`;
  - 58 immediates point at `.dimport_*`;
  - two `bl` go to weak address 0;
  - `generate.py` reads imports (426) from the RPX symbol table.
- **GHS save/restore helpers** at 0x028F5EE0–0x028F626C: epilogues branch into their middles, and
  `generate.py` synthesises those entries (D7). The restore-and-exit helpers return to their
  caller's caller.
- **Recompiler facts from M3:**
  - jump tables are `b` runs;
  - relocations are keyed by symbol (addends exist: `_iob+0x10`);
  - `spr.XER`'s CA/SO/OV copies go stale after a context switch;
  - Cemu's `tw` with TO=0 is its debugger's breakpoint.
- **Reference determinism took patches 0001–0006.** Screenshots of two reference runs can differ
  by a few hundred llvmpipe edge pixels; the traces don't.
- **Cemu's GX2 writes Cemu-only `IT_HLE_*` packets into display lists**, so their sizes differ from
  real GX2. That's why its gx2 front half is ported rather than rewritten (D12).
- **Renderer facts from G2:**
  - the reference's screenshot N is the image its (N+1)th swap presents;
  - WWHD reuses memory for transient targets of other formats within a frame;
  - the shadow map is a 2D array drawn per slice;
  - the bloom chain's mip levels are drawn as separate targets;
  - the G-buffer's normal target is never cleared (the reference zeroes it lazily);
  - the reference's depth clear also clears colour textures at the same address.
- **`Mix(a, b)` in `draw.cpp` is `(a ^ b) * P + C`**, so a key made of two values alone depends
  only on their XOR. Keys that started from two Vulkan handles collided (fixed in e1aae8b). Start a
  key from a hash, or from `Mix(0, a)`.
- **When a run dies silently:**
  - `run.sh` discards stdout, so rerun the program directly with its output kept;
  - Cemu's crash handler writes the stack to `portable/log.txt`;
  - for Debian's libraries (e.g. lavapipe), fetch debug info by build-id from
    `https://debuginfod.debian.net/buildid/<id>/debuginfo` and use `llvm-symbolizer --obj=`.
- **Exits:** `WWHD_EXIT_FRAME` uses `quick_exit`, so end-of-run work registers with
  `at_quick_exit` as well as `atexit`. Closing the window calls `_exit` (the shader cache saves
  first).
- **Profiles from the desktop:** the `exe` line names the desktop's path. Rewrite it to
  `/wwhd/WWHDRecomp/build/play/wwhd-null` (the stripped copy `deploy.sh` made, same addresses) and
  run `tools/profile_report.py` on the worker.
- **Real time vs the virtual clock:** in real time the game is bound to 30 fps and the host has
  headroom; the virtual clock runs flat out (2–3x). The frame-time hitches on a warm start (about
  1 s at boot, about 100 ms loading Outset) are the game's own loading, not shaders.
- **Session scratchpads are temporary.** Put any tool worth keeping in `tools/`.
- **Size and speed:** a full-route trace is about 665 MB compressed. `stream_check route` takes
  about 10 min, `save` about 2.

## Unfinished odds and ends

- An upstream report of nWiiURecomp's missing `fdivs` validator (`xo5==18`) was never posted.
- `symbols.csv` has only 110 names (from the TWW randomizer's linker map). Phase 2 naming is
  needed for M6.
- A segfault inside lavapipe's JIT code was seen once early in a G2 run and not reproduced. The
  handle-key collision fixed in e1aae8b is a candidate cause.
