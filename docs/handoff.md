# Handoff: WWHD recomp, state as of 2026-10-02 (WW-4 in progress)

Read this first, then `CLAUDE.md`, `docs/recompiler-design.md` (decisions D1–D21, milestones,
status paragraphs) and the READMEs in `src/`, `tools/reference/`, `tools/recomp/`, `tools/worker/`.
WW-4's work is on branch `ww-4` (worktree `/srv/projects/WWHDRecomp/.worktrees/ww-4`, based on
`ww-3`), pushed to the `worker` remote.

**The task (the owner's decision, 2026-10-01): native 60 fps, then an uncapped frame rate.**
Not interpolation: the game's own logic runs at 60 ticks a second and comes out right, and in the
end at any rate. **The approach (the owner, 2026-10-02): mixed rate, verified** (D21's option 1):
frames run at 60, everything not yet converted runs exactly as at 30 on whole ticks, and systems
are converted one at a time against a measured baseline. "WW-4: 60 fps" below has where it stands.

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
  `git push worker ww-4` (the current branch). Commit when a step is done and its checks pass.
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
    `tools/reference/cemu-patches/0001–0016` (0011 is the execution seam; 0013 and 0014 are
    rendering fixes, see D16; 0015 lets our CMake build use Cemu as a subproject; 0016 has each
    SysAllocator record its declaration). The same tree is the source of our build's Cemu
    libraries, so a patch there reaches both.
  - To change a patch: commit on the editing machine branch, run
    `git format-patch -1 --start-number N -o tools/reference/cemu-patches/`, sync, then
    `tools/worker/job start cemu-rebuild tools/worker/setup-volume.sh cemu-rebuild` (incremental,
    about 5–10 min). Our forks (`src/forks.txt`) are no longer reached by cemu-patches.
  - Ghidra and its project (`ghidra/projects/`, disposable) are on the worker:
    `tools/ghidra/headless.sh`, `rebuild.sh` (6.4 minutes; set
    `GHIDRA_INSTALL_DIR=/wwhd/opt/ghidra_12.0.4_PUBLIC` in jobs). `tools/ghidra/decompile.py ADDR...
    --out DIR` prints functions as C with callers and callees; `tools/ghidra/lookup.py refs|words|find`
    finds references, dumps words with function names, and finds where a value is stored (vtables
    aren't marked as pointers, so `find` a method's address). Their output is the game's code in
    another form: keep it on the worker (`/wwhd/data/ghidra-out`).
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
- `shader_list_check.sh OUT`: a first start from the shader list prepares everything and changes
  nothing on screen (D20);
- `determinism.sh OUT FRAMES [ROUTE]`; `hle_trace.py summary|dump|diff` (about 65M calls in
  16 s, `--ignore-core`, `--mask-cemu-area`).

**Routes** (`tools/reference/routes/`; input is keyed on the swap count, so routes also play in
real time):
- `title-to-game.txt`: fresh boot → save dialog → title → controller select → file select → name
  entry → the legend intro → Aryll → gameplay on Outset at about f10450;
- `continue-100.txt`: from the owner's 100% save, gameplay on the Outset dock at f870;
- `tour-100.txt`: continue-100, then walk the dock and open the pause menu;
- `sail-100.txt`, `menus-100.txt`, `warp-100.txt` (WW-3, item 5): sailing, the item screen and
  Pictograph Box and sea chart, the Ballad of Gales' warp down to Hyrule Castle.
- `routes.sh` names them all (route, frames, save, baseline trace) for `stream_check.sh`,
  `timing.sh` and `baseline.sh`.

**Baselines** on the worker:

| Baseline | What |
|---|---|
| `/wwhd/data/traces/null-route.zst`, `det-gpu/a.zst`, `det-route/a.zst` | whole route to f10800, **1,124,796,468 calls**, all identical |
| `/wwhd/data/traces/save-det/{a,b}.zst` | save route to f1800, **172,954,163 calls** |
| `/wwhd/data/gx2/{save,route}-cemu.txt` | GPU command streams: 45,955,744 and 143,098,119 packets |
| `/wwhd/data/gx2/{save,route}-audio-cemu.txt` | sound: 5,180 and 30,219 blocks |
| `/wwhd/data/traces/{tour,sail,menus,warp}-det/a.zst`, `/wwhd/data/gx2/{tour,sail,menus,warp}-cemu.txt` and `-audio-cemu.txt` | the newer routes' traces, command streams and sound (item 5) |
| `/wwhd/data/g3/route-ref14`, `save-ref14`, `tour-ref14` | reference captures every 60 frames (title route to f10800, save route to f1800, tour route to f2160), with cemu-patches/0014: the G3 baselines |
| `/wwhd/data/g3/route-vk`, `save-vk`, `tour-vk2` | the renderer's captures of the same frames (G3: all within 60 dB) |
| `/wwhd/data/g3/{sail,menus,warp}-ref14`, `-vk` | the newer routes' G3 captures (item 5) |
| `/wwhd/data/g2/ref`, `/wwhd/data/g3/save-ref` | older reference captures (title route f30–f600 every 30; save route every 60), before 0014 |

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
- `build.sh`: generates the program into `build/recomp` (seconds); the CMake build compiles it.

**Runtime** (`src/`; `src/build.sh` builds it with CMake on the worker: 11-12 min the first time, about 10 s for a one-file change):
- **`build/wwhd/wwhd-null` is the product.** It holds the recompiled program
  (`WWHD_NATIVE=on`), our OS layer, the null GPU and, with `WWHD_RENDER=vk`, our renderer.
  - The null GPU (`src/gpu/null_gpu.cpp`) is a command processor that keeps the register file and
    does every guest-visible effect.
- **Guest memory of Cemu's OS objects** (its 170 `SysAllocator` slots, 0x0E000000 up) comes from a
  table (`src/os/common/sysalloc_layout.h`), by what each slot is, not by static-constructor (link)
  order. A slot the table doesn't know goes after the reference's and is logged; when Cemu or a
  fork's slots change, make the table again (`src/README.md`). Forks replace Cemu's files inside
  Cemu's targets.
- **`WWHD_NATIVE=diff`** is diff mode (D8.2): pure calls and cycles checked against the
  interpreter. Latest on the save route: 1,118,003 checked calls, 0 mismatches.
- **Our OS layer** (`src/os`, src/README):
  - 255 imports are ours: coreinit 31, gx2 179, nn_ac 2, nn_act 2, padscore 6, vpad 4,
    erreula 15, swkbd 16.
  - **30 Cemu source files are forked** (`src/forks.txt`): snd_core; the scheduler and its
    threads, queues, alarms and sync; gx2's core; TCL; proc_ui; the HLE dispatch; the timer;
    fibers; the file system (FS client, IPC driver, FSA service served in place) and nn_save;
    SysAllocator (the layout table).
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
  catches a missing or unlisted override. Three: the game's task loop (D19), the GamePad's screen
  (skipped in real time), and the tick (the 60 fps prototype, D21).
- **Real-time fast paths** (D19): overrides that only run in real time on one host thread
  (`wwhd::rt::FastPaths`; `WWHD_FAST_PATHS=0` turns them off). The task loop (`f_0275FFCC`,
  `src/overrides/task_loop.cpp`) sleeps through the rounds where the game's ticking task only posts
  itself its tick again, proven by the runtime's *quiet watch* (the store journal notes any live
  store). The scheduler thread went from 100% busy to 8–25% on the save route.
- **The renderer** (`src/gpu/vk`, D13; G2 done on the title route): Cemu's shader decompiler to
  GLSL, glslang to SPIR-V, Vulkan 1.3 with dynamic rendering.
- **The shader list** (D20, `src/gpu/vk/shader_list.cpp`, `config/US_v0/shader_list.txt`): a first
  start translates what playthroughs met from the player's game files before the title.
- **The shader cache** (D20, `src/gpu/vk/shader_cache.cpp`), in `portable/shaderCache/wwhd`:
  - it keeps shaders (SPIR-V plus the decompiler facts the draws read), pipeline recipes and the
    driver's `VkPipelineCache`;
  - everything cached is built before the title launches, with a "Preparing shaders" screen when
    that takes over 0.3 s;
  - `WWHD_SHADER_THREADS=n` sets how many threads build pipelines.
- **Playing on a desktop:** `tools/play/deploy.sh` and `tools/play/play.sh` (above).

**Speed** (`timing.sh`, virtual clock, no trace, from launch; the worker's capped i5):

| | Save route (1800 frames) | Other routes |
|---|---|---|
| Before the quick wins | 44 s | — |
| 2026-10-01 | 26.0-26.4 s, 2.27-2.31x real time | sail 2.05x, menus 2.54x, warp 2.22x |

- **Profile of the CPU thread** (save route, virtual clock): guest code 81%, message queues 8%,
  helpers 3%, HLE dispatch 2.5%, gx2 1.5%, thread switches 0.9%.
- **Ruled out:** keeping guest registers in host locals was measured and rejected (the D2 note).
- **In real time on the desktop** (headless, 2026-10-01): save, sail and warp at a steady
  30.1 fps, 99th percentile 34.2-34.8 ms, scheduler thread 7-9% busy (100% before the task loop's
  fast path). On the worker, at the same clocks, the process went from 62% of a core and 4,500
  wakeups a second to 55% and 800 (item 9), leaving the scheduler and GPU threads.

**Which checks for which change:**

| You changed | Run |
|---|---|
| anything the guest can see (OS layer, scheduler, forks, runtime, generated code) | `stream_check.sh save NAME`, then `route NAME`: trace, command stream and sound identical |
| generated code or its runtime helpers | the fuzzer for touched mnemonics; diff mode (below): MISMATCH 0 |
| speed | `timing.sh save OUT` (and `route`), then `python3 tools/profile_report.py OUT/profile.txt` |
| the renderer or the shader cache | `shader_cache_check.sh` (and `shader_list_check.sh` for the shader list or shader keys); `survey.sh` + `compare_frames.py` against the reference captures (threshold 60 dB), or against your own "before" captures (byte-identical when nothing on screen should change) |
| real-time behaviour | headless on the desktop (above): the `wwhd real time:` lines, and a profile |

Diff mode on the save route:
`tools/worker/job start diff env CEMU_BIN=/wwhd/WWHDRecomp/build/wwhd/wwhd-null WWHD_NATIVE=diff
WWHD_RT_LOG=/wwhd/data/traces/diff/rt.log REF_SAVE=/wwhd/data/saves/wwhd_100
tools/reference/route.sh /wwhd/data/traces/diff 1800 tools/reference/routes/continue-100.txt
/wwhd/data/traces/save-det/a.zst`, then `grep "diff (final)" /wwhd/data/traces/diff/rt.log`.

## Done in WW-3 (the record; the owner chose each item in turn)

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

### 2. Fewer pipelines through dynamic state: measured, dropped (owner's choice, 2026-10-01)

`tools/shaders/recipes.py` on both routes' recipes (629 pipelines, played in real time on the
desktop): stencil reference and masks as dynamic state save 0, extended dynamic state 1 and 2
(core 1.3) 5, extended dynamic state 3 24 more; 586 distinct shader pairs are the floor (D20).
Pipelines come from shader variants, not state, so nothing was changed.

### 3. The first playthrough without hitches: done (WW-3, the owner's choice)

- `config/US_v0/shader_list.txt` (D20 "The shader list"): 1382 shaders, 1000 pipelines from the
  save, title, tour, sail, menus and warp routes (the new-game route added nothing); no game content.
  `deploy.sh` ships it to `cemu/wwhd/`.
- A first start translates the listed shaders from the player's own game files before the title
  (2.2 s on the desktop, 2.7 s on the worker's CPU) and builds the pipelines; on the desktop the
  title route then met 1 shader (5 ms), the save route none.
- To grow it: capture runs with `WWHD_SHADER_SOURCES` (tools/shaders/README.md), merge with
  `tools/shaders/shader_list.py`, commit. Check with `tools/reference/shader_list_check.sh`.
- With the sail, menus and warp captures merged, a first start of the warp route and of the
  new-game route (lavapipe, empty cache) prepares everything in 6.4-6.8 s and meets nothing during
  play. Not yet measured on the desktop (it was asleep).
- Ideas: fewer registers per line (the ranges are generous; the capture check would catch a range
  cut too far), and capturing a dungeon proper when a route reaches one.
  For phones: a first start reads 258 MB from the 49 files (the largest, 47 MB, whole in memory
  while it's walked); reading SARC members with seeks would keep that small.

### 4. G3: the scripted routes render within tolerance: done (WW-3, the owner's choice)

- All three routes, every 60 frames, against the reference (D13, D16.3; "G3 status" in the design
  doc): the title route 180 of 180 frames, worst 74.3 dB, 133 identical; the save route 30 of 30,
  worst 91.1 dB; the tour route 36 of 36, worst 74.6 dB.
- The fix was in the reference: Cemu patch 0014 (a draw that samples its own colour target reads the
  image from before the draw, as the renderer and the console's caches do). The owner chose that
  over matching Cemu's in-place reads.
- To check a renderer change: capture both sides (`survey.sh`, or `run.sh` with `CEMU_SHOT_FRAMES`
  for the save and tour routes) and `compare_frames.py --threshold 60` against the `*-ref14`
  captures. For a difference: `compare_dumps.py` at that swap (take a fresh reference dump; old
  dumps may predate a patch), `WWHD_RENDER_TRACE=N:0` for every draw of a frame with its targets and
  textures, `WWHD_RENDER_TRACE=N:ADDR:X,Y` for one pixel.
- Still untested (not on these routes): GPU-side `GX2CopySurface` (only reported), readback into
  linear-special destinations, 3D textures and cube-map targets, depth-stencil textures loaded from
  memory. New routes would show them.

### 5. Extended routes: done on the worker (WW-3, the owner's choice)

- Three new routes from the 100% save (`tools/reference/routes/`, named in `routes.sh`): `sail`
  (board the King of Red Lions, sail into open sea, 2940 frames), `menus` (the item screen, the
  Pictograph Box's view and "album full" message, the sea chart, 1920) and `warp` (the Ballad of
  Gales, the cyclone to the Tower of the Gods, its courtyard, the ring of light down to Hyrule
  Castle, 3780). The route files say what each input does; `warp-100.txt`'s header says how
  conducting works in WWHD (right stick notes, left stick meter).
- Baselines for them and for `tour` (`tools/reference/baseline.sh`): the reference is deterministic
  on each, and wwhd-null's trace, command stream and sound are identical (sail 85,626,017 packets,
  menus 35,664,037, warp 102,492,391, tour 53,853,702). Diff mode on `warp`: 2,427,705 checks, 0
  mismatches. `WWHD_FORKS=0` builds again (weak stand-ins in `src/runtime/without_forks.cpp`).
- G3 on them (every 60 frames, against reference patch 0014): sail 49/49 (worst 90.9 dB), menus
  32/32 (89.3), warp 63/63 (72.2, the beam of light into Hyrule).
- Timing (virtual clock, worker, one host thread): sail 2.05x real time, menus 2.54x, warp 2.22x,
  save 2.24x. One host thread is enough so far (D19 step 4).
- Their shaders are in the shader list (item 3). On the desktop (2026-10-01, headless, after items
  6-10): a first start prepared the list's 1,382 shaders and 1,000 pipelines from the game's files in
  3.3 s; save, sail and warp then ran at 30.1 fps with a 99th percentile of 34.2-34.8 ms and the
  scheduler thread 7-9% busy, meeting no shader or pipeline during play. The first real-time menus
  run met 2 shaders and 1 pipeline the list lacks (real time renders frames the lavapipe capture
  didn't): capture it on the desktop (`WWHD_SHADER_SOURCES`, tools/shaders/README.md) and merge.
  The last 10 s of warp (the beam into Hyrule) and the first of menus dip to 29.5 fps with an
  87-89 ms 99th percentile, most likely the game loading the next area (not checked).
  (Per-thread numbers there: the process is `wwhd` once the frontend names its main thread, so look
  it up by that name.) A dungeon proper
  (enemies, puzzles): from Hyrule Castle (warp route's end, B gets out of the boat), or Dragon Roost.

### 6. Our own CMake build: done (WW-3, the owner's choice)

- `CMakeLists.txt` (root) and `src/CMakeLists.txt` build `wwhd-null` from source: Cemu is a
  subproject from `CEMU_SRC` (the reference's patched tree; patch 0015 makes its CMake files work
  as a subproject), configured as Cemu's own build without its wxWidgets GUI. At configure time
  CemuCafe loses Latte and each fork replaces its original inside Cemu's target. `src/build.sh`
  now just runs CMake, so the old commands still work (`src/build.sh`, `WWHD_FORKS=0 src/build.sh
  build/wwhd-cemu`). `tools/recomp/build.sh` only generates; the CMake build compiles the shards.
- Link order: first a second link with every member in the reference's order; since item 8's
  layout table, one plain link.
- Checked: the CMake binary matches the reference on all six routes (traces, command streams,
  sound; route 1,124,796,468 calls), diff mode on warp is clean, and its menus frames on lavapipe
  are byte-identical to the old binary's. The `WWHD_FORKS=0` build reproduces the baselines.
- First build 11-12 minutes on the worker (Cemu's libraries included); a one-file change about 10 s.
- `wwhd` (Cemu's Latte with our frontend) isn't built any more; Cemu_release is the comparison.
  `frontend/window_system.cpp` still has its Xlib path, unused.
- Not done: other platforms. The flags are clang's; our own code isn't portable yet (the fibers'
  x86-64 context switch, D19's arm64 item). Untested anywhere but the worker.

### 7. The file system and nn_save: done (WW-3, the owner's choice)

- Forked: the FS client, the IPC driver, the FSA service, `nn_save` (`src/forks.txt`, now 29).
- The FSA service is served in place (`src/os/iosu/fsa_service.h`): no IOSU host thread, no trip
  through IOSU's kernel. The FS client serves a request on the calling guest thread and hands the
  reply to its core's IPC thread exactly as IOSU did (a host-style send that readies the IPC thread
  without switching to it, `__OSSendMessageAsHost`); synchronous requests are plain calls.
- Checked: all six routes identical (traces, command streams, sound); the new-game route's 41 save
  files byte-identical to what Cemu's own FS writes (`build/wwhd-cemu`); a real-time run on the
  worker at 30 fps. Not yet on the desktop (offline).
- Left as Cemu's: `fsc` underneath (portable), the client's console-shaped structures, and
  `SAVEInit`'s account lookup through IOSU's legacy act ioctls (with `nn_act`).
- Gotcha: a SysAllocator in a fork stays even when unused (`iosu_fsa.cpp` keeps its old message
  buffer), or every later one moves in guest memory.

### 8. Loader and memory map: started (WW-3, the owner's choice)

- **Done: the guest OS objects' own home.** Cemu's 170 `SysAllocator` slots (its OS objects in
  guest memory, 0x0E000000 up) take the reference's addresses from a table
  (`src/os/common/sysalloc_layout.h`, used by our fork of `Common/SysAllocator.cpp`), by what each
  slot is (declaring file, line for a header, size, alignment, ordinal; cemu-patches/0016 has each
  slot record its declaration). Link order doesn't matter any more: the build links once, and
  `src/link_order.py`/`.txt` are gone. Checked with a plain link: all six routes and diff mode
  identical; the `WWHD_FORKS=0` build (which keeps this one fork) reproduces the baselines.
- Make the table again when Cemu or a fork's slots change: `CEMU_SYSALLOC_LOG=path` from a run
  that matches the reference, then `tools/sysalloc_layout.py` (`src/README.md`). The worker's
  Cemu_release is rebuilt with all 16 patches: its save-route trace is unchanged (172,954,163
  calls) and the slot list it writes itself equals the table.
- **Not started:** our own loader (placing `cking.rpx` as Cemu does: text at 0x02000000, data
  from 0x10000000, import trampolines at 0x00E00000) and our own memory map (Cemu's MMU ranges).
  Both have to reproduce Cemu's addresses exactly; traces will say where they don't.

### 9. Cemu's idle threads stopped: done (WW-3, the owner's choice)

- Measured first: in real time gx2 is 1.2% of the scheduler thread (guest code 83%), so gx2's core
  rewrite isn't a speed lever. Cemu's input manager and audio backend were: four threads (input
  update every 1 ms, Wiimote reader, SDL events, cubeb's PulseAudio) doing nothing for us, 5.4% of a
  core and 2,870 wakeups a second.
- The frontend stops them at start-up (`QuietCemuInput`, `QuietCemuAudio` in
  `src/frontend/window_system.cpp`); snd_core opens no Cemu audio device unless `WWHD_AUDIO=cemu`
  asks or the window's own fails (`AXOut_UseCemuDevices`). Headless runs without the hash are now
  silent rather than playing into PulseAudio.
- Worker, real time, same clocks: 62% of a core and 4,500 wakeups/s before, 55% and 1,640 after;
  only the scheduler and GPU threads are left. Routes identical (save, new-game, warp checked), and
  a windowed run under Xvfb keeps the reference's trace.
- Then the GPU thread (the owner's choice): it no longer wakes every millisecond while waiting, only
  for a submission or the host-timed vsync: 967 wakeups/s to 137, the process 797 (82% fewer than
  at the start), 30.1 fps and a 34.5 ms 99th percentile at full clocks. Save route identical.

### 10. The GamePad view: skipped in real time (WW-3, the owner's choice)

- The game draws the GamePad's screen every frame though no GamePad is attached: the **ITEMS
  menu** (3D item icons, tabs, buttons) into an 854x480 colour buffer (864x480 with depth), copied
  to the GamePad's scan buffer (`GX2CopyColorBufferToScanBuffer` r4=4). Save route: 194,754 of
  2,136,689 draws (9%, about 108 a frame). The TV's pause menu is a separate 1920x1080 render.
- The draws come from the engine's generic UI/layer renderer, shared with the TV's HUD: on a task
  thread (core 2, `task_MessageLoop` → handler `f_0276AA34` → `f_027588DC` → `f_027D74AC` →
  `f_027C424C`) and on the main render thread (core 1). Those functions are among the hottest
  (8-12% of CPU samples inclusive each), but the GamePad's share of them is only known by draws.
  No GamePad-only function is on the path; the data says which screen a view is for.
- With the Ghidra project rebuilt (`tools/ghidra/rebuild.sh`: 6.4 minutes on the worker; it
  reproduced `functions.csv` and `jump_tables.csv` but for the names `symbols.csv` had gained) and
  `tools/ghidra/decompile.py` (prints functions as C with callers and callees; its output stays on
  the worker, `/wwhd/data/ghidra-out`): the render jobs draw each scene's views through
  `f_027D6BB0` (now `gfx_RenderSceneView`), which passes the scene's render target, a float
  rectangle, to `gfx_RenderView`. There are two targets: the TV's (0,0-1920,1080) and the GamePad's
  (0,0-854,480). Eight engine functions are named in `symbols.csv` with that evidence.
- The override `src/overrides/gamepad_view.cpp` (D9) returns without drawing for the GamePad's
  target in real time only; `WWHD_SKIP_GAMEPAD=1` forces it with the virtual clock, `=0` turns it
  off. Forced: the save route's draws 2,136,689 to 1,941,934 (exactly the 864x480 ones gone, the
  TV's counts unchanged to the draw), and the menus route's 32 TV frames byte-identical to the
  unskipped render. Gain: 9% of the draws (a whole 854x480 pass with depth: GPU on a phone) and
  about 1% of CPU (virtual clock 26.3 s to 26.0 s on the save route). Without forcing, the checks
  run the game's code: save, menus and warp identical, diff mode clean.
- Tools left: `WWHD_BACKTRACE`, `WWHD_SHOT_DRC` (`src/README.md`), `tools/ghidra/decompile.py`.

### 11. 60 fps (M6): research started (WW-3); WW-4 carries it on

See "WW-4: 60 fps" below and design D21.

## WW-4: 60 fps (in progress)

**Done (step 1 of the owner's plan, research; committed b14d39f):** WWHD's frame mapped down to
every process's execute and draw; where the per-tick steps are (a study of the GameCube decomp);
prior art (the Wind Waker Recomp's experimental 60 Hz mode is the closest); the probe that showed
the game runs at 60 frames a second with its logic held to whole ticks; the options. D21 has it all.

**Step 2, the measuring tool and the baseline (the owner: finish it on every route before
converting; D21 "Step 1"):**
- Tick rules (`config/US_v0/tick_rules.txt`, 39) hold the game's logic to whole ticks; the random
  stream is saved and put back around half ticks; the rollback (level 2, the default) undoes what
  the main thread writes on half ticks into processes and the game's .data/.bss; three per-frame
  sead nodes are held to whole ticks (`WWHD_60FPS_NODES`).
- With the draw pass on half ticks (what converted systems need): save, tour, sail and menus are
  exact (every actor, every tick); warp is exact through the cyclone, the Tower of the Gods and
  the descent into Hyrule Castle, and two actors differ in a few fields near its end (the pirate
  flag's cloth packet, a Moblin byte), without spreading. Sound requests are serviced up to half
  a tick sooner (the sound engine runs per frame).
- With nothing of the game's frame on half ticks (`WWHD_60FPS_HALF=none`): exact on the tour
  route too, and whole ticks' pictures are pixel-identical to the 30-tick run's.
- Left: the two warp fields; per-effect sound timing (a hook on the game's sound calls); the title
  route (boot and menus at 60 aren't measured: runs switch at swap 900, in play).

**Step 3, conversion (the owner: the camera and Link first, measured at each stage, then they try
it in a window; D21 "Step 2"):**
- Stage 1, the machinery (done): converted processes (`WWHD_60FPS_CONVERT=476,168`) run every
  frame with a time step (`g_rtStep` 0.5), the rest on whole ticks; 18 helpers with a time step
  (`src/overrides/sixty_step.cpp`); step rules in the generator (`keep`, `*h`, `/h`, `k`, `d`,
  `split`, after an instruction or `@` for it only); the step-doubling trial
  (`WWHD_60FPS_TRIAL=476,168`, `tools/sixty/trial.py`), which finds a process's unconverted
  per-tick code one tick at a time; `tools/sixty/rmw.py`; per-tick tracking
  (`WWHD_STATE_TRACK`, `compare.py --track`); stores name their guest instruction to the journal.
  Checked with the switch off: the fuzzer on every store mnemonic, both routes' traces, command
  streams and sound, diff mode.
- Stage 2, the camera (rules in, measured with the trial): `updateMonitor`, `Run`'s counters, the
  bank's damping, the forward cushion, `followCamera` and the HD port's own engine (`f_0250FDC8`).
  Left: the eye's coupling (D21: convert the direction's factors with the eye's 0.75 together),
  `bumpCheck` and the shake.
- Stage 3, Link (rules in, measured at 60 on every route): `posMoveFromFootPos` (with the new
  `note:`/`arc@` rules for the arc), `posMove`, `setNormalSpeedF`, overrides for `checkPass`, the
  old pose's blend, the byte timer, the collision status's countdown, and two whole-tick
  registrations. Converted processes fall back to whole ticks during events (and on a half tick
  after one is ordered), and button presses read once a tick. The feet's and the camera monitor's
  moves are measured over a whole tick; the camera's direction factors are converted with the
  eye's (`k75`). Worst errors: tour 24 (camera 1.2 degrees on average), menus 0, warp ok (one
  scene starts a tick later); sail: the camera holds through the boat's turn (mixed rates: the
  ship isn't converted, D21). The real-time fast paths and the half ticks now share the store
  journal (only real time sees it).
- Stage 4, the boat (done): about 35 rules (execute, `setYPos`, `setWaveAngle`, `setMoveAngle`,
  `setControllAngle`); the camera's type is chosen on whole ticks (`whole:r3=r4`). Sail: the boat
  within 84 units of the 30-tick run, the camera's view within 1.3 degrees on average.
- A half tick's draws see the whole tick's globals (converted processes' half-step writes hidden
  from the draw pass, handed back after), and the HUD (481) isn't drawn on half ticks: booting at
  60 crashed every time in its text code (its half-tick draw rebuilt text, the rollback put back
  dangling pointers); the HD port renders the HUD every frame anyway. Booting at 60 now runs
  clean, in real time too. Real time: 60.1 fps, frame times median
  16.7 ms, 99th 18.6 ms (tour, camera + Link + boat converted, desktop headless).
- The owner's feel test (2026-10-02, in a window on the desktop; D21 "Step 3"): smooth facing the
  sea; facing Outset a chug and slow motion; the sail and the wake flickered; was the boat too
  fast? Answers and fixes:
  - The boat's speed per tick equals the 30-tick run's (33.06/33.06, 49.94/49.93 units).
  - The chug: frames over 16.7 ms waited a whole extra vsync (the game is frame-locked). The half
    tick's journal now filters by page and saves each byte once a frame (it was a fifth of the
    main thread); the renderer hashes shader programs once a frame and, in real time, samples
    textures (whole textures in turn) instead of hashing all of them every frame (half of the
    renderer thread); and `src/overrides/pacing.cpp` paces by pairs (a late whole-tick frame
    doesn't wait for the next vsync; the half tick's frame waits for the pair's second). Sail
    route, desktop, headless: 53.3 fps boarding with the island in view, now 60.1, no late frames.
  - The sail is GRID (171): rules in `tick_rules.txt`, converted with `WWHD_60FPS_CONVERT=...,171`.
  - The wake, bow waves and splashes are particles, calculated on whole ticks: next, the particle
    system converted.
- Real-time measuring: `WWHD_FRAME_LOG=path` (every frame's work, GX2DrawDone wait, both threads'
  CPU, vsyncs missed; `tools/sixty/frames.py` summarises), `WWHD_PROFILE` (`tools/profile_report.py`
  on the worker with the same build). On the desktop, `~/wwhd-test` is ours to deploy to and run
  headless; `~/wwhd-play` is the owner's.
- Then D21's order: particles, animation-heavy actors, the HUD, then actors route by route;
  uncapped. Tools: `tools/sixty/track.py` (fields tick by tick, half ticks too),
  `tools/sixty/camera.py` (the camera's view, 30 against 60).
- Run converted: `WWHD_60FPS=1 WWHD_60FPS_CONVERT=476,168,165,171` (with `WWHD_60FPS_FROM` for routes).

**How to measure** (all on the worker; dumps are game memory, keep them there):
- `tools/sixty/run.sh tour /wwhd/data/m6/NAME` runs 30 and 60 (`WWHD_60FPS_FROM=900`,
  `WWHD_VIRTUAL_SPEED=3`) with the state probe; `python3 tools/sixty/compare.py OUT/30 OUT/60
  --from 880 --names /wwhd/data/ghidra-out/actor_names.tsv` lines them up by game frame.
- `WWHD_STATE_DUMP_GLOBALS=t1,t2,...` dumps every actor and all of .data/.bss at those ticks;
  `WWHD_STATE_CENSUS=1` (half ticks) or `2` (every frame) with `WWHD_STATE_CENSUS_CHAINS=1` or
  `WWHD_STATE_CENSUS_TRACE=addr` finds who writes what (`tools/sixty/census.py`).
- `WWHD_60FPS_HALF=none` skips the game's frame on half ticks (the exact reference: whole ticks'
  pictures equal the 30-tick run's; half ticks repeat the frame before). `WWHD_60FPS_ROLLBACK=0..3`
  sets the rollback's reach (2 is the default); `WWHD_60FPS_SKIPDRAW=n,m|all` skips those
  processes' draws on half ticks (a probe); `tools/sixty/probe_menu.sh` runs one such probe on the
  tour route and reports the menu's timing.
- `SIXTY_TRACE=snd_core.` records each run's OS calls of that prefix (`hle_trace.py dump`).
- Finding code: `tools/ghidra/source_files.py` (functions by assert file), `tools/ghidra/disasm.py`,
  `decompile.py`, `tools/profile_tree.py` with `WWHD_PROFILE_DEPTH=64` (whole call chains),
  `/wwhd/data/ghidra-out/actor_profiles.tsv` (the 449 actor profiles).
- Converting: `WWHD_60FPS_TRIAL=476 tools/sixty/run.sh tour /wwhd/data/m6/NAME 30`, then
  `python3 tools/sixty/trial.py /wwhd/data/m6/NAME/30 --target 476` (what's left, by instruction);
  `tools/ghidra/disasm.py ADDR... --out /wwhd/data/ghidra-out/asm` and `python3
  tools/sixty/rmw.py ASM.s` (a function's read-modify-write stores); then rules in
  `tick_rules.txt` with the decomp's line as evidence. At 60: `WWHD_60FPS_CONVERT=476
  WWHD_STATE_TRACK=476 tools/sixty/run.sh tour OUT`, then `compare.py OUT/30 OUT/60 --track`.
- Adding an override rebuilds all generated code (12 min); a tick or step rule rebuilds one shard;
  `tools/recomp/runtime/ppc_ops.h` rebuilds everything.

**Keep in mind:** behind the switch (`WWHD_60FPS`) every check is unchanged (checked: both
routes' traces, command streams and sound, diff mode). Android: 60 ticks doubles the game's CPU,
phones throttle; converted systems only cost what they convert.

## Waiting on the owner

- **The sound check** of the task loop's fast path (item 1): they listen in a window when they have
  time ("Launching the window" above).
- **The menus route's 2 shaders and 1 pipeline** missing from the shader list (item 5): a
  real-time menus run on the desktop with `WWHD_SHADER_SOURCES`, when the desktop is free.

## After that, roughly in this order (ask the owner)

- **The rest of D18:** the loader and memory map (the rest of item 8); gx2's core rewritten (no
  speed in it: 1.2% of the CPU thread, item 9); proc_ui.
- A dungeon route (from Hyrule Castle at the end of the warp route, or Dragon Roost).
- Longer task-loop naps (item 1's idea): fewer wakeups, with care.
- M5, playable on a GPU machine (the desktop is one); an arm64 context switch for Android (D19).

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
- **Real time on the worker needs a busy core beside it.** Its governor (`powersave`) keeps our
  half-busy, often-sleeping thread at low clocks: 27-28 fps and a 64 ms 99th percentile. With
  `timeout 120 sh -c 'while :; do :; done' &` running alongside: 30.1 fps, 34.7 ms. Measure frame
  pacing on the desktop; per-thread CPU and wakeups on the worker come from `/proc` (D19, "Idle
  threads and clocks").
- **Session scratchpads are temporary.** Put any tool worth keeping in `tools/`.
- **One `wwhd-null` at a time per directory.** Our frontend keeps its portable folder (NAND, saves,
  log, shader cache) next to the binary and ignores `CEMU_PORTABLE`, so a second run of
  `build/wwhd/wwhd-null` shares the first one's saves. For a second run alongside, copy the binary
  to a folder of its own (e.g. `/wwhd/data/g3/bin-dump/`). The reference Cemu has its own folder.
- **Rendering on lavapipe is slow:** about 40 frames a minute at 1080p with two runs side by side,
  so the whole title route takes about 4 hours per side.
- **Size and speed:** a full-route trace is about 665 MB compressed. `stream_check route` takes
  about 10 min, `save` about 2.
- **The CMake build:**
  - `src/CMakeLists.txt` lists our sources by name; a new file must be added there.
  - Cemu's `find_package()` targets are global (`CMAKE_FIND_PACKAGE_TARGETS_GLOBAL`), but its
    `pkg_check_modules()` ones aren't (libusb). Our object libraries only take what they can see.
  - `CemuResource` (the system font coreinit hands the game) used to come in only through Cemu's
    GUI, so the executable links it itself.
  - Every Cemu patch changes the tree's git hash, which Cemu compiles into every file
    (`EMULATOR_HASH`). So reconfiguring after a patch recompiles all of Cemu, in the reference's
    build and in ours.
  - A `WWHD_FORKS=0` build is a separate build directory and a full compile.

## Unfinished odds and ends

- An upstream report of nWiiURecomp's missing `fdivs` validator (`xo5==18`) was never posted.
- `symbols.csv` has 131 names: the TWW randomizer's linker map, the task library, the render path
  and the frame loop (WW-3). Phase 2 naming (zeldaret/tww) is what the 60 fps work will lean on.
- A segfault inside lavapipe's JIT code was seen once early in a G2 run and not reproduced. The
  handle-key collision fixed in e1aae8b is a candidate cause.
