# Handoff: WWHD recomp, state as of 2026-10-03 (WW-4 in progress)

Read this first, then `CLAUDE.md`, `docs/recompiler-design.md` (decisions D1–D21, milestones,
status paragraphs) and the READMEs in `src/`, `tools/reference/`, `tools/recomp/`, `tools/worker/`.
WW-4's work is on branch `ww-4` (worktree `/srv/projects/WWHDRecomp/.worktrees/ww-4`, based on
`ww-3`), pushed to the `worker` remote.

**The task (the owner's decision, 2026-10-01): native 60 fps, then an uncapped frame rate.**
Not interpolation: the game's own logic runs at 60 ticks a second and comes out right, and in the
end at any rate. **The approach (the owner, 2026-10-02): mixed rate, verified** (D21's option 1):
frames run at 60, everything not yet converted runs exactly as at 30 on whole ticks, and systems
are converted one at a time against a measured baseline. "WW-4: 60 fps" below has where it stands.

## Parallel sessions (from 2026-10-03; the owner's setup)

WW-4's actor conversion is split between two sessions working side by side:
- **Session `top`**: worktree `/srv/projects/WWHDRecomp/.worktrees/ww-4-top`, branch `ww-4-top`,
  worker checkout `/wwhd/WWHDRecomp-top`. Takes the work queue from the top down.
- **Session `bottom`**: worktree `.worktrees/ww-4-bottom`, branch `ww-4-bottom`, worker checkout
  `/wwhd/WWHDRecomp-bottom`. Takes the queue from the bottom up.
- Each worktree has `.worker-dir` and `.session` (gitignored): `tools/worker/sync.sh`, `w` and `job`
  use that worker checkout (its own `build/`), `job` prefixes job names with the session, and
  `tools/sixty/tests/*` write to `/wwhd/data/m6/<checkout>/`. Never touch the other session's
  worktree, worker checkout, jobs or outputs. One build at a time each: the worker is shared.
- **The queue** is `tools/progress/plan.json` "queue" (areas in story order), shown on the progress
  page (http://WORKER_ADDR:8765). Before starting an item: `tools/progress/publish.sh claim ID
  "what"` (it refuses an item the other session holds); when its types are converted, checked and
  integrated: `publish.sh done ID "summary"`. Stop when the next item is the other session's. At
  each step `publish.sh now "TEXT"`; after each integration `publish.sh` (the numbers).
- **Rules go in per-area files**, `config/US_v0/tick_rules/<area>.txt` (the generator reads them
  after `tick_rules.txt`; an address may be ruled once in all of them), so the sessions never edit
  the same rules file. Docs: append to your own subsection of "WW-4: 60 fps" below.
- **Integration**: `ww-4` (on the `worker` remote) is the shared branch. To integrate a finished
  step: commit on your branch, `git fetch worker && git rebase worker/ww-4`, resolve (the
  default list `kConvertedByDefault` in `src/overrides/sixty.cpp` has one string literal per
  session: append to your own line), rebuild, run `tools/sixty/tests/checks.sh NAME` (all must match) and, after shared changes
  (helpers, shared rules), `tools/sixty/tests/regress.sh`; then `git push worker HEAD:ww-4` and
  `git push worker HEAD:ww-4-<session> --force-with-lease`. If the push to ww-4 is refused, the
  other session integrated first: fetch, rebase, check again.
- Shared code (sixty.cpp, sixty_step.cpp, generate.py, ppc_ops.h, the helpers): change it only
  when needed, say so in the commit, and rerun `regress.sh`: the other session's actors use it too.

### Session `qa` (from 2026-10-04): the owner's bugs

A third session fixes the bugs the owner finds when playing, while `top` and `bottom` carry on:
worktree `.worktrees/ww-4-qa`, branch `ww-4-qa`, worker checkout `/wwhd/WWHDRecomp-qa`, session
name `qa` (same `.worker-dir`/`.session` mechanism, same integration into `ww-4`).
- **The bug list** is on the progress page, kept with `tools/progress/publish.sh bug ...`: `bug add
  TITLE [DETAILS]` (one per problem the owner reports; it prints B1, B2...), `bug start ID` before
  working on it, `bug note ID TEXT` for findings (the cause, the commit that brought it in),
  `bug fixed ID "commit, how"` once the fix is in `ww-4` and deployed, `bug verified ID` when the
  owner confirms, `bug reopen ID` if not, `bug wontfix ID "why"`. `bug list` prints them.
- **The owner's copy**: from now on only `qa` deploys to `~/wwhd-play` on the desktop
  (`tools/play/deploy.sh owner@DESKTOP_ADDR`, after a build of the current `ww-4` tip with the
  checks passing), so what the owner plays is always the integrated state; say in the bug notes
  which commit is deployed. `top` and `bottom` deploy only to `~/wwhd-test` (headless). Don't
  touch the owner's saves or shader cache; open a window only when they ask.
- **Finding a cause**: the switch-off checks prove 30 is unchanged, so a bug at 60 comes from a
  conversion, a rule or shared 60 fps code. Narrow it with `WWHD_60FPS_CONVERT` (the default list
  minus a type, or empty), `WWHD_60FPS=0`, `WWHD_60FPS_EVENTS=0`, `WWHD_60FPS_ATTACHED=0`,
  `WWHD_60FPS_PLANTS=0`; `git log -S`/`git log -- config/US_v0/tick_rules/<area>.txt` names the
  commit and session. Reproduce headless on the worker (routes, `WWHD_DEBUG_STAGE`/`SPAWN`/`BOSS`,
  `tools/sixty/tests/*`, captures with `shots.sh`, `framediff.sh` for 30-looking stretches) or on
  the desktop's `~/wwhd-test`. For what only happens in hands-on play, ask the owner for an F9
  flight recording: deploy with `WWHD_FLIGHT=$HOME/wwhd-play/flight/f WWHD_STATE_TRACK=<types>` in
  its `play.sh` environment (see "The flight recorder" below); copy recordings to the worker.
- A fix in a type's rules goes in the rules file that holds them (whichever session's); a fix in
  shared code needs `regress.sh` and a note in the commit, as for the others. Tell the owner in a
  short line which bugs are ready to retest.

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
  - The wake, bow waves and splashes are particles: the particle system is converted
    (`WWHD_60FPS_PARTICLES=1`: the 3D particle calc every frame with a step, JParticle's steps in
    `tick_rules.txt`; the wake's and bow waves' strips emit every frame). Particle counts match the
    30-tick run's. The wake's last flicker was its texture scroll, which truncated the half-step
    frame count (`int tick = getFrame()` in JParticle's texture matrix): the new `reload` step rule
    reads the float back at 60 (`g_rtSixty`, drawing too). Now the wake changes every frame alike.
    The sea's CPU waves are flat in WWHD (the GPU makes them), so the sea isn't converted; its
    ripple texture's scroll counted every draw (twice as fast at 60) and now counts whole ticks.
- The owner's second try (2026-10-02): A did nothing. The window, started from a remote session
  with `systemd-run`, had no focus on Wayland, and SDL drops a controller's events then; the
  gamepad now plays without focus (`SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS`), and the log notes
  focus changes. Scripted input was never affected (the game reads the controller on whole ticks
  only, so no press is lost to a half tick: checked with a probe). Booting at 60 logs six sead
  asserts (J3DDrawBuffer entries, from Link's title-screen draw, and one more), this morning's
  build too and with nothing converted; not at 30: open.
- The owner's third try (2026-10-02/03, Steam closed: Steam Input had taken the controller). Their
  list: the camera overshoots and snaps back whenever it has far to swing (shield, sidling, a
  ladder); torches' flames flicker; carried things (pots, bombs, rocks, the arrow on the bow)
  flicker as Link walks; trees and bushes being cut, seagulls, enemies and Dragon Roost's lava,
  rocks and platforms move at 30; songs and doors drop to 30 (events: converted processes hold to
  whole ticks there, by design so far). The owner's order: these bugs, then events at 60, then
  actors.
  - **The camera's overshoot, fixed.** The owner's recipe: sword out, turn round, ZR (the shield;
    ZR is Crouch/Defend, R an item button, ZL lock-on). It's the shield camera (`f_02513E98`, work
    area tagged 'SHLD'), an engine with no rules: a ramp by (N - m11C) / W whose weight W lost a
    tick's share on both frames, so the factor passed 1 (at 60 the camera swung ~145 degrees past
    Link's back, then came back). Rules in `tick_rules.txt`; at 60 the half frame now lands on the
    30-tick run's view every tick (route `drc`, the owner's save `/wwhd/data/saves/owner_drc`, on
    the worker only). The camera's tick counters (Run's m07C, m11C, m118, m080, m108, added to
    after the engine) and the follow camera's ramp weight are now `late` (a new tick rule: once a
    tick at its end, the half frame while stepping, so both frames see the tick's count), and the
    first-person camera's ramp (`f_025071FC`, 'SUBJ': the bow, the telescope) is converted: it
    went in front-loaded and a tick early, now evenly and on the 30-tick run's tick.
  - **The flight recorder** found it: `WWHD_FLIGHT=path WWHD_STATE_TRACK=476,168` keeps the last
    20 s (`WWHD_FLIGHT_FRAMES`) of the tracked processes and the controller in memory; F9 in the
    window (and the exit) writes `path-TIME-N.bin` (track.bin's form; the pad as process 0xFFFF).
    Copy to the worker, never keep on the editing machine. Routes made while looking: `shield`, `swing`,
    `land`, `ladder` (unfinished: it doesn't reach the ladder), `drc`.
  - **The torches' flicker, fixed.** Not the flames: every other frame the room's walls and floor
    were drawn without the actors' real shadows (a 4x4 placeholder for the shadow map, so the
    torchlit wall's look flipped). The HD renderer draws that pass only in a frame that asked for
    it, and the asking is in the environment update (`f_0255E854`), which a tick rule held to whole
    ticks; it now runs on half ticks too (its globals and the random stream are put back after a
    half tick; the 60-fps run's actor states are unchanged, 0 of 49589). Found with render traces
    (`WWHD_RENDER_TRACE=F:0`, then the wall draw's textures) and by freeing frame-level tick rules
    one by one. Probes added on the way: `WWHD_60FPS_DRAWDIFF=swap` (what a frame's draw pass
    changed in .data/.bss, `WWHD_60FPS_DRAWDIFF_OUT`), `WWHD_60FPS_FIND=swap:v,...` (where MEM2 holds
    those words, `find.txt`), `WWHD_60FPS_WATCH=addr,...` (words at each frame's first draw,
    `watch.txt`), both written in the binary's folder.
  - **What Link holds follows his hands.** Carried pots, bombs, rocks and the arrow on the bow
    were drawn on half ticks where his hands had been (their executes run on whole ticks). Link's
    four actor keeps (+0x6590: ID, actor; equip (the arrow), throw, grab, rope) are noted after his
    half step; each held actor's execute then runs again as at 30 just before its half-tick draw,
    and what it changed is put back right after that draw; its draw's stores are journaled too
    (the arrow's draw sets the bow's charge in Link, a converted process the journal otherwise
    skips; Link draws what he holds inside his own draw, so the journal is switched on there).
    Checked: the arrow stays on the string every frame as at 30 (route `bow`); the 60-fps run's
    states are identical with it on and off (0 of 47005; `WWHD_60FPS_ATTACHED=0` turns it off).
    Not checked on a pot: scripting a lift from the owner's save failed (route `pot` stops short:
    A by the jar didn't lift; the owner's play will tell).
  - **Events at 60, begun.** Converted processes now step at 60 in an event too while Link's
    action (daPy_lk_c +0x65F0, its function at +0x65F8) is one checked in events: 4 wait, 6 move.
    Walking out of an entrance (route `door`, the owner's save) then matches the 30-tick run per
    tick, camera too. The Wind Waker (actions 0x9A-0x9C, `f_0243A094` procTactPlay: a beat phase
    +0x69F8 and timers +0x69FC/+0x6A00/+0x6A04 stepped 1 a tick, a countdown +0x6918, the notes from
    the stick) isn't converted: with it stepping the song failed, so its actions stay off the list
    (the song still works, at 30). `WWHD_60FPS_EVENTS=0` turns event stepping off.
    Then converted: conducting (0x9A, `f_0243A094`: the beat phase and three timers `*h`, the
    countdown `whole`) now steps too; the camera follows the 30-tick run's exactly through the song,
    the warp map comes up and the route ends as before; the song ends 2.5 ticks early (a beat lands
    on the half tick it crosses). Next: door-opening and talking actions; cutscenes (dDemo's frame).
  - Open: the warp route at 60 logs a burst of 30-40 sead asserts in one frame late in the route
    (J3DDrawBuffer's `p_pkt->getEntryPtr() == 0`, from Link's draw), with nothing converted too;
    none at 30. The half tick's draws: to look into (the same assert as the six at a boot at 60).
  - **Seagulls** (KAMOME, 194, d_a_kamome.cpp) converted: position adds `*h`, the tick count and six
    timers `whole`. They fly smoothly at 60 at the 30-tick run's speeds; their paths drift apart over
    time (integer angle approaches round differently by half steps, and they pick random targets), as
    any wandering actor's will.
  - **Bokoblins** (BK 189, d_a_bk.cpp, with their sticks, BOKO 463) converted, a first pass: the
    enemies' shared per-tick update (`f_02041F94`: a tick count and four timers, `whole`) and
    shared move (`f_02043F34`: position += speed K + the forward speed, the knockback, gravity, all
    `*h`) serve every enemy that uses them; the Bokoblin's own tick counts and countdowns `whole`
    and two float accumulators `*h`. Fighting Link (placed beside them with `WWHD_DEBUG_PLACE=
    1000:-1075,0,5700` on route `drc`) they cover 1734 and 1424 units in 290 ticks against 1605 and
    1365 at 30 (their steering drifts); the trial has nothing left stepping twice but angles that
    chase a moving Link. Not yet tried: getting hit and dying, picking the stick up.
  - **Plants** (the scene's grass, trees, bushes and flowers: packets the play scene updates, not
    processes) run every frame at 60 now (`executeGrass/Tree/Wood/Flower` overrides; tick rules held
    them to whole ticks): the trial along route `plants` found two sway phases a tick (`split`).
    `WWHD_60FPS_PLANTS=0` keeps them at 30; `WWHD_60FPS_TRIAL_PLANTS=1` (at 30) is their trial.
    Cutting a bush or a tree isn't tried yet.
  - **Dragon Roost's mountain path** (route `door`, walking out of the dungeon): the lava geysers
    (Obj_Ygush00 151), lava (Obj_Eayogn 154), Obj_Gryw00 (142), the flags (Tori_Flag 175, a cloth
    wave: two phases `split`), bomb flowers (296, d_a_bflower.cpp) and Obj_Ebomzo (162) converted:
    tracked at 60 they match the 30-tick run tick for tick (half frame k against tick k) but for
    the sound source's listener-relative position, which follows the camera.
  - **Which types need no rules**: `WWHD_STATE_TRACK=all WWHD_60FPS_CONVERT=all,-168` (every actor
    converted but Link, so that what reads Link sees the 30-tick run's) and `python3
    tools/sixty/actor_types.py OUT/30 OUT/60 --from 905 --names actor_names.tsv` rank every type by
    the ticks it matches. On routes `door` and `bk`: push blocks, chests, doors, the sky and five
    tags match at every tick (the visible ones are in the defaults now); the mountain set ~93%; stones
    and the warp object differ only in colliders 3.75 units higher (a resting actor's speed.y is
    -g h, not -g); torches (EP) in their light's flicker, drawn from the random stream a step (left
    at 30: converted, it would jitter twice as often); pots, steam vents, Valoo, the Rito and idle
    Bokoblins: to look into.
  - **Test aids (the owner's idea of a debug menu, its first pieces)**: `WWHD_DEBUG_STAGE=tick:NAME,
    point,room,layer[;...]` changes stage at a game frame the way an exit does (g_dComIfG_gameInfo's
    next stage at +0x5140: name[8], s16 point, s8 room, s8 layer, s8 enabled, u8 wipe; the current
    stage's name is at +0x5134). From the Outset dock `920:M_NewD2,0,0,-1` lands in Dragon Roost
    Cavern's entrance; rooms 1-4 of M_NewD2 with point 0 land in four other places (room 3 outdoors at
    night: a layer?). `WWHD_DEBUG_PLACE=tick:x,y,z` moves Link within a stage.
    `WWHD_DEBUG_SPAWN=tick:process,param,x,y,z[;...]` creates an actor there in Link's room
    (the creation record `f_025D5678`, then fpcM_Create `f_025E14A8` on the layer at *0x101F3AE8):
    a Bokoblin at `950:189,0,-201622,190,312600` stands on the Outset dock behind Link and acts.
    `WWHD_DEBUG_BOSS=1` answers "no" to every stage's "boss beaten" (dSv_memBit_c::isDungeonItem
    item 3, f_025B9100; isStageBossEnemy and 44 call sites read it), nothing written to the save:
    with `920:M_DragB,0,0,-1` Gohma is back in its room on the finished test saves (all attached saves
    are 100%), its fight started (no intro: that is the boss door's event). The start of a boss rush.
    Next: an in-game menu on top of these.
  - **Enemies by spawning** (`WWHD_DEBUG_SPAWN` on the Outset dock, the scratch script's steps: spawn,
    trial, 30 against 60 by half frame): Keese (KI 215: its fly's position += speed and a velocity,
    `*h`; a tick count) now fly the 30-tick run's path within 7 units (8642 against 8637 units);
    Chuchus (CC 206: its timers) walk it within 1-7 units, their jump a tick early. Both in the
    defaults. Magtails (MT 216): counters, timers, phases and four move modes have rules, but on a
    wooden dock it falls into the sea and the 60 run takes another branch: test in its lava, not in
    the defaults. Moblins (MO2 188), Darknuts (TN 191) and ReDeads (RD 224) already walked 30's
    path (their moves go through converted helpers): only their counters and timers needed rules,
    one of them a shared countdown helper (f_0211D2F8, 173 callers). Kargarocs (BB 181): its mover
    f_0205E638 (pos += speed; a timed rise) takes half steps; it flies 30's path within 7.5 units.
    A trap found there: `pos.y += x` where x is a snap to a height above the ground, not a per-tick
    amount (02061118): halving it made it sink. Check what is added before `*h`: a store whose
    value at 30 moves once and then stays is a snap. All four in the defaults. Still to do: Stalfos
    190 and Poe 212 (drift), Wizzrobe 208 (no trial data: probably did not spawn with param 0),
    Octorok 227, 184, 220. An enemy that makes random choices on half steps takes the next tick's
    numbers: its path drifts from the 30-tick run's even when right.
  - **Enemies in their own rooms** (`/wwhd/data/m6/stagetest.sh PROC STAGE,point,room,layer FROM TO`:
    the warp at f920, the trial, 30 against 60). Rooms found by warping and listing processes:
    Magtails in M_NewD2 rooms 8, 10, 12; the Forbidden Woods (stage `kindan`) room 9 has Peahats,
    Boko Babas (BO 214, idle with Link far), 213 and process 204, room 11 has 27 of process 205.
    Magtails track 30 in their lava within 10 units until a random wait ends differently: in the
    defaults. Peahats (PH 209): their state timer +0x462 (21 sites) whole, the spin and wobble phases
    `spliti` (new: an `addi`'s immediate split between the half ticks), height `*h`; they fly 30's
    path within 7.8 units: in the defaults. 205 (rides a parent: pos += the parent's move, then
    cLib_addCalc2 toward a target) moves ~3/4 as far at 60: not in the defaults. 204 is a Mothula
    (GM, "gmos" in the room data; it was taken for a Floormaster, d_a_fm.cpp, at first): its state timer, counters, gravity and spins have rules; it dashes as at 30 but its
    dashes end at other times (232 units apart on average): not in the defaults.
    Boko Babas (BO 214): rooted; spawned beside Link (`WWHD_DEBUG_SPAWN=1000:214,0,-278,6052,-6700`
    after the warp to kindan room 9; `WWHD_DEBUG_PLACE` doesn't hold there: Link's execute puts him
    back at the spawn point). Measured by the head (eye point +0x390: `/wwhd/data/m6/eye.py`): its
    timers whole, the idle sway tracks 30 within 1-2 units, a lunge comes at another moment (its
    timing is random). In the defaults.
  - **NPCs** (`/wwhd/data/m6/routetest.sh ROUTE PROC FROM TO`: trial, then 30 against 60 on top of
    the defaults; Outset's tour route 950-2190). NPC_YM2 (316) matched; NPC_YW1 (317) needed only
    the shared s16 countdown (cLib_calcTimer<s16> f_02055B64, now `whole`) and tracks within 9.7
    units: both in the defaults. NPC_SO (118, the one leaping out of the sea) leaps higher at 60
    from the first jump: to do. Found on the way: cLib_addCalcAngleL (f_0200F474, s32) had no
    time-step override; it has one now in `sixty_step.cpp`, and a circling point (f_02587128:
    angle += speed) gets `split`. `actor_types.py` now also skips +0x194 (listener-relative too:
    static signposts differ there).
  - **Windfall, converted (session top, 2026-10-04; rules in `config/US_v0/tick_rules/windfall.txt`)**.
    `tools/sixty/tests/types_test.sh sea,0,11,-1 1000 1500` (every actor converted at once but Link)
    found 15 types matching at every tick and 18 not; `area_test.sh` (the trial and 30 against 60 for
    several types in one run), `actor_rmw.py` and the decomp did the rest. In the defaults now:
    - the windmill's gondola wheel Obj_Ferris (123): its angle `split`, the gondolas' sway `spliti`,
      the frame timer `late` (rot_mng reads it first), the start/stop states' counters; at 60 its angle
      equals the 30-tick run's at every half frame;
    - the flags MAJUU_FLAG (174; the rules are session bottom's in ganon.txt, the same seven derived here
      independently): a 21-point cloth, speed += force (`vec@` on PSVECAdd), speed *= 0.85
      (`d@` on PSVECScale), position += speed (`vec@`), its wave phase; it flapped twice as fast (points
      moving 51 units a tick against 24), now 26 against 24 (the cloth's shape diverges: chaotic);
    - pigs KB (220): timers, the push-out WWHD adds by hand (`*h`), the roll, flips, bob and anger flash
      of their other states (from the decomp); their gait matches (speedF 1.33/1.32, walking 45%/45%,
      steps 2.98/3.03 units a tick) while their paths diverge with random choices;
    - townsfolk: Mila NPC_KK1 (353: blink, three frame counts; 31 units off before, 4 now), Ivan NPC_MK
      (170) and the Killer Bees NPC_UK (368: blinks, visit timers, gravity, a sidestep impulse; 2-7
      units), Mila's father NPC_GK1 (374), Tott NPC_TT (364), NPC_PEOPLE (373, sixteen) and Zunari
      NPC_RSH1 (352) (blinks, countdowns); and the static ones that match: market stalls Obj_Roten (121),
      the island's distant models (445) and stands DAI (309).
    Blinks and random waits differ from the 30-tick run in phase, not rate (the random stream differs
    at 60). Not converted: pots TSUBO (453: resting ones match, but thrown, rolling and floating ones
    have ~30 per-tick sites: an item of their own), Coming2 (270: the sailing barrel course's invisible
    manager), item stands (462), salvage lights (401), shutters (259), door knobs (305), the mailbox
    (67), Obj_Light (128: phases that run at night only), tags, and 27, 38, 195, 298, 384, 439 (matched
    in daytime, other states unchecked). NPC_SO (118) is on Windfall too: Outset's item.
  - **whole, late or keep for a counter** (session top): which is exact depends on when the code reads
    the value within the tick. Changed first and read later in the same execute (enemy timers at the top
    of execute): `whole` on the store. Read at entry and changed after (`if (t) { t--; break; }`, the
    NPC blinks' `if (frame >= max) ... else frame++`): `late` (with `whole` the half tick sees the next
    tick's value and acts half a tick early). Changed and tested on the register in the same call
    (`t++; if (t > N)`): `keep` on the add (otherwise the half tick computes t + 2 from memory and can
    cross early). `t++; if (t == N) act()` where acting changes no state: `whole` on the action as well
    (the pig's lift particle). `python3 tools/sixty/rule_audit.py` (worker) lists every `whole`/`late`
    store whose register is tested, returned or put in cr0 right after: it found the shared countdown
    helpers (cLib_calcTimer<s16> f_02055B64 and the int one f_0211D2F8, 173+ callers) returning `*t - 1`
    on half ticks while `*t` stayed, so callers waiting for 0 acted half a tick early and then again;
    they, Darknut's countdown, a Moblin, Floormaster and NPC_YM2 counter now have `keep` on the add
    (2026-10-04, shared: regress.sh rerun; the u8 one's override already returned `*t`). The regress
    set showed two more, fixed: the enemies' shared move (f_02043F34) adds speed.y K to the position
    before gravity (explicit Euler), and with gravity `*h` two half steps fell K g/4 a tick too far
    (0.75 units, so a Moblin walking off the dock was 7 units lower 10 ticks on): its speed.y store is
    `late` now, gravity once a tick, and the fall matches the 30-tick run's (a tick early: it reaches
    the edge half a tick sooner); and the Chuchu's own `speed.y += gravity` (021101B0) had no rule, so
    its jumps had half their height and airtime. Left: an enemy falling into water hangs at the surface
    about 2 ticks at 30 and not at 60 (the move's ground check), and hand-made gravity with `*h` arcs
    about g/4 a tick higher than at 30 (fopAcM_posMove's exact arc only knows fopAcM_calcSpeed's
    gravity: a rule that hands posMove the gravity added by hand would make them exact). Also shared,
    found on Windfall: the light palette's start timer (setLight_palno_get,
    `config/US_v0/tick_rules/env_light.txt`), counted in every converted actor's draw, so twice a tick.
  - **Outset, converted (session top, 2026-10-04; rules in `config/US_v0/tick_rules/outset.txt`)**:
    `types_test.sh - 950 2190 tour` found 16 types matching at every tick; the rest with `area_test.sh`
    outdoors and, after a warp, in Link's house (`LinkRM`) and Abe and Rose's (`Onobuta`; `Omasao` has
    lamps, an item and a chest; `Pjavdou`, `A_mori` and `LinkUG` were listed too; `Ojhouse` didn't
    load). Unnamed process numbers named from the decomp's process list (`f_pc_name.h`: WWHD's are the
    GameCube's minus 1-3 here): 200 crabs KN, 255 items ITEM, 407 Lwood. In the defaults now:
    - the fishman NPC_SO (118): its blink, and its pull toward the circling point (pos += (point - pos)
      mB04: `k@f1` on cXyz operator*): the half steps had moved it 1.64 times as far. Its jumps were
      not higher at 60 on the tour route (peaks 733-1078 against 860-1206 at 30: random speeds);
    - Beedle's ship OBJ_IKADA (68): its bob, the lantern's spin, the rocking phases (`split`), WWHD's
      own count (`whole`: its phase is the count x 0x900, an integer, so it steps once a tick); the
      sea's rocking is the shared dLib_waveRot (`config/US_v0/tick_rules/dlib.txt`, two phases `spliti`);
    - palm trees Obj_Lpalm (73): the trunk's slerp toward the wind (`k@f1` on C_QUATSlerp's t) and the
      fronds' wave phases (wind x 0x800 plus a random jitter a tick: `split`);
    - crabs KN (200): the timers at the execute's top (walks lasted half as long: paths 495 against
      1047 units) and the hand-made gravity on three paths;
    - items ITEM (255): timeCount's counters and changeDraw, the flashing before an item vanishes (a
      toggle when m_timer % the cycle is 0, so `whole` on the call: both frames of a tick saw that
      m_timer and toggled it back);
    - the grotto OBJ_HOLE (71: a billboard to the camera) and the mailbox OBJ_TORIPOST (67): no rules;
    - indoors: Grandma NPC_BA1 (335), Joel and Zill NPC_KO1/KO2 (319, 320), Rose NPC_OB1 (331)
      (blinks, `keep`), the dishes Obj_Mshokki (461: damping, spins, a counter) and shelves Obj_Shelf
      (45: the shake's timer and phases, the fall's speed, decay `k@`, wait).
    Not converted: wall lamps (LAMP 186) and torches (EP): a random flicker target every step would
    flicker twice as often (as the earlier note on torches); signs KANBAN (182: cut pieces' physics),
    pots, stones; A_mori's 204/205 (the forest item's types).
  - **The Great Sea, converted (session top, 2026-10-04; rules in `config/US_v0/tick_rules/sea.txt`)**: rooms
    from `tools/stage_actors.py sea` (the room data: placements by process number, all story layers);
    `types_test.sh` and `area_test.sh` after warps to rooms 25 and 37 (cannon rocks, warships, Bokoblin
    platforms, Gyorg waters), Link placed beside an Octorok (room 18) and a Gyorg spawner (37) with
    `WWHD_DEBUG_PLACE` (positions from `stage_actors.py --pos`). In the defaults now:
    - cannons OBJ_CANON (63): the barrel's sway (a phase in the joint callback, `split`) and a counter
      while the boat's cannon fires: 99.7% of ticks equal the 30-tick run's (0% before);
    - warships OSHIP (179): the attack sway's phase on two paths; within 8 units;
    - Gyorg spawners GY_CTRLB (230): a phase on two paths (100% after, 0% before);
    - Obj_Coming (269): its entries' countdowns; lookout platforms Obj_Aygr (164), wind tags (391) and the
      ships' flags Sie_Flag (176: a dCloth, its fixed values set at creation): no rules;
    - bombs BOMB (294: Link's bombs and the cannonballs): the fuse (`keep`), the no-gravity and shadow
      timers (`late`), the collider's mass time, a counter, water damping and floating, the wind's
      damping. A bomb spawned on the dock lives the same 93 ticks at both rates. A wind-pushed bomb
      moves by daObj::posMoveF_grade, time-stepped now (session bottom's override, 8e84743).
    `trial.py` now reads integer fields' changes modulo their width: s16 phases that wrap (the cannon's,
    the spawner's) had come out as x-8.58 and x-326 and been filtered away as noise.
  - **Forest Haven and the Forbidden Woods (session top, 2026-10-04; rules in `config/US_v0/tick_rules/forest.txt`)**:
    rooms from `tools/stage_actors.py Omori` / `kindan` (it now reads layer tags like ACTa). On the 100%
    save Forest Haven has no Koroks (they have left for the islands). In the defaults:
    - Forest Haven: fireflies FF 187 (77 of them: the timers, the glow's phase, the move, a vector add:
      `vec@`), forest fireflies NH 313 (speed.y moved toward maxFallSpeed by gravity: `*h`; paths 451/455
      against 451/455 units, apart 0.3-0.6; were 469/473 and 6.8), lily pads LEAF_LIFT 120 (counts, the
      tilt's slerp `k@`, WWHD's ripple count), baba buds JBO 213 (the launch countdown, the wobble `split`,
      the launch flag on Link once a tick: buds poked into their wait, `WWHD_DEBUG_POKE=1000:213,3d8,1,1;
      1000:213,3da,2,46`, count and wobble as at 30), trees Lwood 407 (WWHD sways them by a tick count),
      and with no rules Obj_Ojtree 83, WARPFOUT 105, the Deku Tree NPC_DE1 116 (idle only), KUI 250 (in
      session bottom's line: the tower's stakes), KYTAG00 383, TAG_HINT 410, BG 439, Tag_Attention 470;
    - the Forbidden Woods: vines SK 101 and SK2 102 (their joints are sines of a count taken before it
      counts: `late`; `whole` had them a tick ahead), fences SAKU 398, acorn leaves ACORN_LEAF 297 (`keep`:
      counts to 69, compared at once), and with no rules warp pots OBJ_WARPT 65, KDDOOR 304, leaf piles
      Obj_Leaves 144, Obj_Mtest 74, ANDSW0 307, 430.
    - then: ceiling tentacles SHAND 99 and small vines SSK 103 (below), Mothulas GM 204 (one more count,
      `keep`: the trial clean, 5669 units flown against 30's 5750 in room 9 on the walk route; its dashes
      are random) and Kalle Demos (kinBOSS, `WWHD_DEBUG_BOSS=1`, walk route): the core BMD 235 (timers,
      phases, `pos += speed` and the thrown core's `m924 += m930` by `vec@`, its move's explicit gravity
      `late`; 14 units from 30's path, was 119), the ceiling tentacles BMDHAND 236 (20; 7-23 units over
      ~5000-unit paths, were 72-190 and 7% longer) and the floor tentacles BMDFOOT 237 (8; they ride the
      core: 5-24 units apart, paths 5-10% shorter as the core's 5%).
    The general tentacle (SHAND, BMDHAND: "汎用触手") is a one-pass chain: every joint is put back at its
    length from the joint before's new place, its direction the old one plus a pull (a rotated vector) and
    a wiggle (sines of a tick count) added each tick. Both added terms h of themselves (`vec@r3` on the
    pull's MtxPosition, the new `*h:` on the wiggle size's load, in each copy of the loop: SHAND 2,
    BMDHAND 7) brought SHAND within 0.4 units of the 30-tick run. (Session bottom found Verlet chains,
    where a move crosses one segment a step, can't be half-stepped: they keep those updates `whole`.)
    Vines' counts: SK's joints read the count before it counts (`late`), SSK's first joint takes it from
    the register right after (`keep`), SK2's countdown decides by its `extsh.` (`keep`):
    `tools/sixty/rule_audit.py` found that one and Windfall's ferris speed step (`keep` now).
    Not converted (forest.txt has the sites found): hanging flower platforms KITA 97 (floating in water
    they drift and turn a step a tick), the hanging house KOKIIE 98 (its fall), Octoroks OQ 227 (the
    bob's phase `spliti` at 023C11AC; attacks untested), Morths 205 (random hops; a background helper
    moves them 1.63 times as far), torches, pots. Process 204 is a Mothula (GM, "gmos"), not a
    Floormaster: corrected in tick_rules.txt.
  - **Torches and wall lamps (session top; `config/US_v0/tick_rules/objects.txt`, the leftovers both
    sessions split once the queue was done: pots and stones are session bottom's, `carried.txt`)**:
    torches EP 185 (the flame's flicker counts timers down and picks a random target and timer at 0;
    the picks' stores `whole`, as a timer picked as 0 would pick again on the half tick; the glow's
    spins `spliti`; their two moths' timer, random target, step and push `vec@`, wings `spliti`) and wall
    lamps LAMP 186 (the sway's count `spliti`, the quake shake `late`, the wind's hit timeout `keep`):
    trials clean in Link's house and the Forbidden Woods but for a torch light's radius (a cube of its
    power: 0.19 a tick against 0.11) and the moths' wing scale and turn (read before their phase moves:
    half a tick ahead). Both in the defaults.
  - **Session top's leftovers, tested (2026-10-04; rules in objects.txt, sea.txt and forest.txt)**:
    - signs KANBAN 182, converted: cut in a test by spawning one ahead of Link (Forest Haven, after a
      warp to `Omori,0,0,-1` Link stands at 2113,714,-1335 facing -x:
      `WWHD_DEBUG_SPAWN=1000:182,0000034d,2033,714,-1335`) and the new route `slash` (B at f1010 and
      f1030). The mother and pieces: timers, gravity copied into every state's tail (`fall@`), the
      float's step, the fallen mother's wobble, the pieces' spin. Trial clean; a tracked piece 21 units
      from the 30-tick run's (81 before);
    - Octoroks OQ 227, converted: the bob's phase, their spin (csXyz `+=` on whole ticks) and a count.
      In the Forbidden Woods' pools (Link placed by them in kindan room 12) they woke: trial clean, two
      within 1.5 units of the 30-tick run (paths had been twice as long); at sea the one in room 18
      stayed under, Link swimming 600 units off;
    - Seahats (PH 209 at sea, already in the defaults): their sea code's three phases and a step (`split`,
      `vec@`); ten in sea room 24 came from ~3800 units apart to 17-31 (one 224);
    - hanging flower platforms KITA 97 and the hanging house KOKIIE 98, converted: their sways verified;
      the platforms' water drift and the house's fall aren't reached on the 100% save (the house lies
      at its landing height from the start), ruled from the code;
    - Morths 205, converted: the sideways wiggle `*h`, the inlined gravity `fall@`, and the hops' impulse
      and sound on whole ticks (a Morth landing on a half step hopped again on the next one, half a tick
      early). On dry ground with Link going straight (kindan rooms 6 and 16, route walk, f1000-1080) their
      paths go both ways around 30's (489/511, 568/677, 529/474: random hops and headings); the trial has
      only derived positions and the wall push-out. In room 11 Link sinks and swims, and the camera's swim
      turn differs at 60 (session bottom's note), so the Morths there part from 30's;
    - Big Octo 225/226 and Gyorgs 228, converted, tested on the boat (route `sail`, aboard by f1095,
      sailing west-northwest from f1400): Big Octo's switch (0x0D) is set on the 100% save, so one is
      spawned ahead of the boat without it (`WWHD_DEBUG_SPAWN=1450:225,ffff2802,-216800,0,314300`): trial
      clean, its body within 18 units of 30's as it pulls the boat (its swallowing is an event: whole
      ticks). Gyorgs need the ship (their spawner targets it; one spawned alone deletes itself): a type-B
      spawner spawned ahead of the boat (`1450:230,ffff1441,-214000,0,312500`) makes three; trial clean but
      for copies, paths within 2.5-4.5% of 30's. (sea.txt has the notes.)
    - `WWHD_DEBUG_PLACE` didn't hold at 30 in Forest Haven (Link stayed at the spawn at f1000 and f1050)
      though it did at 60: a test placing Link compares different scenes then; spawn the actor in front
      of Link instead (the spawn aid works at both rates).
  - **More leftovers (session top; split with session bottom from its census of placed types: rope bridges
    BRIDGE 89 are bottom's)**: converted, rules in sea.txt and objects.txt:
    - floating barrels Obj_Barrel2 457 ("Ktarur", sea rooms 23 and 44): three damped springs, each
      `v = (v + force) d; p += v` once a tick (the float's height, the drift home, the tilt toward the
      water's normal; off the open sea the normal's own random sway). `keep` on the velocity's fmadds/fsubs
      and fmuls (the generator's `keep` now takes the A-form multiplies too: shared, `generate.py`), `*h@`
      on `p += v`: p is the 30 Hz one at half ticks. The bob's random phase `split`; the ram cooldown
      `keep`; the explode and cutscene countdowns (`addic.` then a branch) `late`, and so are the
      cutscenes' event orders: the boat's crash is seen on a half tick (collisions resolve on whole
      ticks), and with the order held to whole ticks it was never made before the countdown gave up (the
      barrel broke without its cutscene, 51 ticks early). Tests: sea room 23 (paths 251/247, 289/287,
      365/363 units; trial clean), and barrels spawned in the sail route's way (the boat at
      (-212612, 311604) by f1600): a type 1 the boat pushes (its dip and settling like 30's; the push
      itself lands a little differently, as collisions see whole-tick positions), a type 0 it rams (the
      break cutscene at both rates, deleted at f1646 and f1647; the boat ends within 12 units of 30's).
      The spawned barrels sit at different heap addresses at 30 and 60, so paths.py can't pair them;
    - the heat haze Ykgr 397 (Dragon Roost Cavern, Fire Mountain's cave): its strength's approaches are
      stepped; its alpha (a static byte) fades a step a tick, `split`. Routes `door` and `drc` and Cave01
      rooms 0 and 1: trials clean, fields equal to the last bits (it follows the camera's eye);
    - floor switches Obj_Swpush 27 ("Kbota"): the top's spring as the barrels' (`keep`, `*h@`), counters
      `keep`. A momentary one spawned under Link on route `save` (`950:27,0100ffff,-201622,138,312243`)
      bounces through a 60-tick cycle with Link on it, height and speed equal to 30's at every tick;
    - fires Fire 424 (Cave01 room 1's six) and warp lights WARPLIGHT 106 (Cave01 room 0): two countdowns
      `keep` and the event orders `late` (made every tick until the event starts); 100% equal to 30's.
    - lava (shared: `sixty.cpp`, `overrides.txt`, `tick_rules.txt`): 434 makes a floor in the scene's
      dMagma_packet and deletes itself (no process), and the packet's calc, `executeMagma` (f_02524CA0),
      was held to whole ticks by a frame-level rule. It is an override now, as the plants are (their
      helper is now `ScenePacket(ctx, fn, converted, trial)`): every frame at 60 with a time step,
      `WWHD_60FPS_MAGMA=0` keeps it at 30, `WWHD_60FPS_TRIAL_MAGMA=1` (at 30) is its trial (read it with
      `trial.py --all`: its stores are globals and heap, which trial_filter drops). Steps in objects.txt:
      the scroll and the glow's color cycle `*h@`, the bubbles' phase `spliti`, and a bubble's respawn
      (random) `whole` with its resets, since the split phase wraps on a half step. Trial in Cave01 room
      1: clean but for the scroll's last place;
    - Not converted: the Hyoi seagull NPC_KAM 195 (idle, it matches; its flight with a Hyoi Pear isn't
      tested).
  - **Two 60 fps crashes in the Forbidden Woods, fixed (session top; shared, `sixty.cpp` `LiveStore`)**: at
    every warp there with Link converted (the defaults). Both came from half ticks putting back or hiding
    words in .data/.bss that head lists whose nodes are on the heap (which a half tick never puts back):
    - J3DDrawBuffer.cpp 243 (`p_pkt->getEntryPtr() == NULL`, 4 OSPanics): a half tick's frameInit took
      Link's static eye packet (`l_offCupOnAupPacket`, .bss) off its list; the rollback gave the packet its
      list slot back (+0x94); the next entryImm saw it entered. `WWHD_60FPS_ROLLBACK=0` had none;
      `WWHD_60FPS_SKIPDRAW=all` had hundreds;
    - then a segfault in JAISeMgr (f_0280593C, the game stopped at tick 1029): a converted Link's footstep
      started on a half tick changed the sound lists' heads (0x104B5008), which the draw pass saw hidden
      and got back after it. `WWHD_60FPS_HIDE=0` had none.
    Now what J3DDrawBuffer's list code (frameInit, the entries, J3DPacket::clear) and JAudio's JAI layer
    (f_02801444 to f_0280E03C) write is neither put back nor hidden: they are live like the heap. Rooms 5,
    9, 11 and 12 run to the end at 60. Other lists headed in .data/.bss with heap nodes would fail the
    same way (the census probe and `hidden.txt`, written by `WWHD_STATE_DUMP` runs at exit, list the
    hidden words).
  - **Link: two counters (session top found them in the lily pad test)**: Link's +0x6980 (`023FBEE8 addi`,
    + 1 then compared to a virtual call's value) and +0x424 (`023F2E30 sth`, his cutscene move's timer)
    ran twice as fast; ruled now (`keep:r29`, `whole`) in `config/US_v0/tick_rules/link_items.txt`.
  - **Tools (session top)**: `tools/sixty/tests/types_test.sh STAGE FROM TO` (an area's first pass),
    `area_test.sh STAGE FROM TO P,Q,...` (several types in one run), `capture.sh NAME STAGE SWAPS
    [INPUTS]` (captures for the progress page at 60 on the worker's GPU: quick, not for comparisons);
    `python3 tools/stage_actors.py STAGE [ROOM...] [--layers]` (worker) lists every room's placed actors
    by WWHD process number from the game's files, all story layers (WWHD's stage and room archives are
    SARC, some inside content/Common/Pack's packs, and embed the room.dzr / stage.dzs in their .bfres;
    the names map through l_objectName, found in the RPX). The sea's: Big Octo DAIOCTA 225 in rooms 6,
    17, 20, 36; Gyorg spawners GY_CTRLB 230 in 5, 37, 42, 47; Octoroks OQ 227 in 18, 25, 27, 38, 41, 48;
    warships OSHIP 179 and cannons OBJ_CANON 63 in ~17 rooms each. `--pos PROC` prints a type's
    placements (where to put Link with `WWHD_DEBUG_PLACE`). Two tests at once: `SIXTY_OUT=DIR` and a
    `CEMU_BIN` copy in a folder of its own (one binary's runs share its emulator dir, NAND and log).
  - **Doors**: on the door route at 60 every frame changes up to the black screen of the room load
    (captures of each swap, `/wwhd/data/m6/framediff.sh ROUTE FIRST LAST`); Link and the camera
    step on every half frame. The owner's "entrances drop to 30" wasn't reproduced there: ask which
    entrance.
  - **Don't rule the shared helpers**: cLib_addCalc and kin, fopAcM_posMove, gravity, animation
    frames are already overrides with a time step for every converted process
    (`src/overrides/sixty_step.cpp`). Step rules on them scale twice (tried: every converted enemy
    drifted, Peahats stopped taking off). Rules belong in the actor's own code.
  - **Progress page** (the owner's request): `tools/progress/` (README there), served from the worker
    on the tailnet at http://WORKER_ADDR:8765. After each step: `tools/progress/publish.sh`
    (data) and `publish.sh now TEXT`; captures via `publish.sh shot PPM CAPTION` (they stay on
    the worker). Keep `tools/progress/plan.json` (milestones, known issues) current.
  - **Converting an actor**: `uv run tools/sixty/actor_rmw.py PROCESS` lists the type's
    read-modify-write stores with suggested rules (review them: `+= 1` can be a state machine's next
    step, not a tick count); `WWHD_DEBUG_PLACE=tick:x,y,z` puts Link beside it; the trial and a
    30-against-60 track (compare half frame k of the 60 run with tick k of the 30: a converted
    process reaches a tick's state at its half frame) decide.
  - Link turning at 60 ends up ~10 units ahead: an action that ends with its animation (the turn)
    can end on a half tick, so he starts walking a tick early (route `door`, ticks 1004-1006). Left
    as is: holding such switches to whole ticks would stall him half a tick each time.
- **Session bottom** (from 2026-10-03; the queue from the bottom: `ganon`, then `wind`, `earth`...;
  rules in `config/US_v0/tick_rules/ganon.txt` etc.):
  - **The Ganon area's stages** (room lists at f1095 after a warp at f920): `Hyrule` (the castle's
    courtyard: 13 flags MAJUU_FLAG 174, 16 Lwood 407 (static: a counter only), Moblins, a Darknut,
    Peahats, Keese, Chuchus), `Hyroom` (inside: Triforce boxes 44, torches, pots, the statue YLzou 145),
    `kenroom` (the Master Sword's chamber), `GanonA`-`GanonN` (the tower's halls and its four trials:
    Bubbles BL 207, Wizzrobe 208, Stalfos 190, Poes 212, Darknuts, Moblins...), `GanonJ` (Phantom
    Ganon's maze, rooms 0-4, 6-11, 13), `GanonK` (Puppet Ganon: BGN 243, BGN2 244, BGN3 245),
    `GTower` (Ganondorf GND 246, Zelda PZ 210: an event runs from the warp on), `Xboss0`-`3` (the
    refights: Gohma, Kalle Demos BMD 235 with 236/237, Jalhalla, Molgera). Unmatched process numbers
    were named from the decomp's draw priorities (`f_pc_draw_priority.h`; WWHD's are the GameCube's
    minus 2-3 in this range; zeldaret/tww is cloned at `/wwhd/opt/tww` on the worker).
  - **Converted**: Hyrule's flags (MAJUU_FLAG 174: a cloth of 21 points stepped with force, damping
    and move per half step; they flap at the 30-tick run's speed (points move 5.7 a tick at 30, 6.1
    at 60) but, being chaotic, drift apart in shape after a few seconds), the capes (MANT 192, for
    Darknuts and Phantom Ganon: a position-based chain, so its forces take h^2 of themselves, the new
    `*hh` step rule; on a caped Darknut spawned on the dock the cape's shape relative to its roots
    stays within 10 units of the 30-tick run's on 60% of ticks, diverging when the Darknut itself
    acts differently, and moves ~20% more), the Moblins' lanterns (KANTERA 193: no rules).
  - **Puppet Ganon** (GanonK; BGN 243, BGN2 244 the spider, BGN3 245 the snake; ~110 rules: timers
    and tick counts whole, wobble phases and spins split, the strings' and limbs' chains pulled h a
    step, the snake's body a Verlet chain like the capes, the jumps' arcs, the Keese/Morths it calls
    in once a tick). Its first form with Link standing in the room: entrance, dances and both punches
    on the 30-tick run's ticks (its random dance targets differ). The spider (reached with
    `WWHD_DEBUG_POKE`, below): the same actions on the same ticks, jumps within a few hundred units of
    30's arcs. The snake: moves at 30's speed (49,650 against 49,891 units in 1000 ticks), its chase
    turns elsewhere after ~5 s. The ropes' waves come from its tick count (whole): they move once a
    tick. Not tested (the fight with Link): cut strings, damage, its own attacks after a cut.
  - Test aids: `WWHD_DEBUG_SPAWN=...,x,y,z,ANGLEX` (hex: a Darknut's equipment is (ANGLEX >> 5) & 7,
    `80` a shield and a cape). `WWHD_DEBUG_POKE=tick:proc,offset,size,value[;...]` (offset and value
    hex) writes a process's field before it executes (process 0: the offset is an address, written once
    that frame; `900:0,1046f0b9,1,5;900:0,10474c6b,1,2d` puts the boomerang on X, `900:0,1046f0ba,1,13;
    900:0,10474c6c,1,2f` the hookshot on Y: the save's inventory slot and the play's item number on the
    button, which Link reads; the items route's header has more): e.g. `1100:243,14eb4,2,6;1100:243,14eb6,2,0`
    sends Puppet Ganon into its change (the spider ~f1740), `2000:244,602,2,6;2000:244,604,2,0` the
    spider into the snake (~f2250). `WWHD_DEBUG_BOSS=1` with a Darknut spawned on the Outset dock
    crashes the game at f979 (at 30 too): keep it for boss rooms.
  - **Ganondorf** (GTower, GND 246; the decomp has stubs only): route `gtower` (A every 20 frames
    from f1000) through his arrival's cutscene, which ends ~f8220; then Link rolls about while he
    attacks. Rules: his hair (a Verlet chain like the capes), his walks and leaps (`pos_move`
    f_02154C34: found with `WWHD_STATE_CENSUS=1 WWHD_STATE_CENSUS_TRACE=addr`, which names who wrote a
    field on half frames: the trial had barely seen it, he mostly stood still at 30), his timers.
    Phantom Ganon (FGANON 241?) appears in no room listed at f1095 (GanonJ's maze, M2ganon): left to
    the fortress item.
  - **Miniblins** (PT 247, GanonN) and **Bubbles** (BL 207, GanonB): timers, a hover's phase, falls;
    they move at the 30-tick run's speeds (paths 4040/3747, 4072/3905, 1901/1883 units) and wander
    elsewhere (random-driven).
  - **Doors' rattle** (f_0252B2D8, a helper of DOOR10, which the defaults convert, and DOOR12): its
    count doubled at 60 and the shutters of the tower's battle rooms ended their rattle 20 units off;
    it runs on whole ticks only now (a vibration a tick).
  - **The Wind Temple** (`wind`; `config/US_v0/tick_rules/wind.txt`): stage `kaze` reaches its rooms
    by spawn point (`kaze,P,0,-1`, P 1-19: the room number alone doesn't move Link), Molgera's arena
    is `kazeB`. New routes: `walk` (walk about after a warp: wakes a room's enemies), `kazeb` (into
    Molgera's arena; she rises ~f3200). Converted: the fans (WINDMILL 114: a joint callback adds the
    spin to the blades' angle, `split`), blade traps (Obj_Trap 135: timers; their pause count needed
    `keep`, as the count, not the stored value, decides when they move: move 33 / pause 11 ticks as
    at 30), Wizzrobes (WZ 208: timers, the fade's +-8 `spliti`), moving platforms (MACHINE 254: no
    rules, 1455 against 1455 units), 252 (a wave from its tick count), Armos and Armos Knights (AM
    202, AM2 203, named by their code's place: within 0-4% of 30's distances), Molgera (BWD 217, BWDG
    219: tick counts, timers, a wobble's phase; her pace matches, the fight then diverges with
    Link's hits). Floormasters (FM 119: rules here, converted with the Earth Temple: one matches 30
    exactly, the other drifts with a random speed it draws, cM_rndF(9) + 1; tick_rules.txt's "fm"
    rules are process 204's, not FM's). Not converted: Makar (NPC_CB1 334: moves twice as far
    converted), springboards (166: tiny), the propeller switch (430).
  - **The Earth Temple** (`earth`; `config/US_v0/tick_rules/earth.txt`): stage `M_Dai` by spawn point
    (`M_Dai,P,0,-1`, P 0-18), Jalhalla in `M_DaiB` (WWHD_DEBUG_BOSS=1). Most of these classes have
    WWHD's fields at the GameCube's + 0x11C. Converted:
    - Stalfos (ST 190): timers, its tick count, its own move (speed_pos_calc: pos += speed, then speed.y
      -= 5: the explicit order, so gravity `late`, once a tick, and the two half steps of h speed add up
      to the 30 Hz step), its bones (part_posmove the same way). M_Dai,15's waits in a coffin for a
      switch: `WWHD_DEBUG_POKE=990:190,3e0,2,1` wakes it (WWHD_DEBUG_SPAWN of a Stalfos crashes). Its
      body follows 30 within 7 units until a random wait ends differently. Its hair (three ten-segment
      chains) and loincloth are Verlet chains solved in one pass: with half steps (forces h squared,
      damping h of a power) they swung wider and settled slower, because in one pass a motion moves one
      segment down a step (twice as fast at 60) and the projection's loss depends on the step; an
      offline model of the loop showed the same. So they keep the 30 Hz step: the hair's
      call on whole ticks, the cloth's own stores on whole ticks (its anchor and the mace every step).
      Now the hair's droop and motion match 30 (tip 8.9 against 9.0 units a tick).
    - Poes (PW 212; its code is mostly not decompiled): timers, inlined gravity (fall@, three
      copies), its alpha's sway and other phases, its fade, its lantern's swing phase (kantera_calc
      in the Poe). Paths within 3-8% of 30 (they turn at random).
    - their lanterns (KANTERA 193, in the defaults since the ganon item): its tick count (the swing is
      a sine of it), a dropped lantern's fall (explicit order: `late`), its moths: two lanterns match
      30 exactly, two within 3%.
    - Floormasters (FM 119; rules in wind.txt).
    - Jalhalla (BPW 211, one class for the body, its lantern, the damage ball and the Poes it carries):
      its ten timers, a decay, gravity (13 copies of the execute's calcSpeed), alpha_anime's and
      fuwafuwa's phases, the lantern's swing, a random wait, spins and the skulls it pulls and blows
      (from the decomp). Its body and lantern stay within 2-9% of 30's paths over 800 ticks; its fight
      past the start was not exercised much (the test route only swings the sword).
    Not ruled on purpose: mode/state numbers (PW's mMode +0x484, BPW's mActionState +0x562: `+= 1` is a
    step to the next state, once; a `keep` there would drop a step taken on a half tick), path-point
    indices and random turns: actor_rmw.py's `whole` suggestions include these, check each against the
    decomp. Not converted: Medli (NPC_MD 367: her code moves positions a tick, but this save has no
    companion Medli to test), torches (EP 185: a random flicker target each step, as session top found
    for Outset), tapestries (289: a cloth), mirrors (272), coffins (159), the rope bridge (89),
    light switches (30), pots.
  - **The Forsaken Fortress** (`fortress`; `config/US_v0/tick_rules/fortress.txt`): `MajyuE` (outside: its
    spawn points move Link; its layers list the same actors on the finished save), `majroom` (rooms 0-4
    by point and room), `M2ganon` (Ganondorf's room, Tetra). `tools/stage_actors.py` (session top's)
    lists a stage's placements by layer; a PLYR reader in the same style shows a stage's spawn points.
    Converted: anchors (IKARI 431: a sway from its tick count: equal to 30), barrels (Obj_Barrel 456:
    a countdown; 3.75 units higher at rest, the known resting offset), ropes (HIMO3 447: counters, and
    the moths around their lanterns: 10.00 against 9.96 units a tick) and Tetra (NPC_ZL1 426: idle).
    Not converted: searchlights (OBJ_SEARCH 66) and the pirate ship (57): still on this save (nothing
    to gain); the pirate flag (173) and the sails (SAIL 172): cloths (see the Stalfos' hair: a one-pass
    solver can't take half steps). Rats (NZ 198, not decompiled; here and in the tower) are converted
    since: they run 20 a tick at both rates; their timers (keep) and their tail (a one-pass chain: its
    call on whole ticks) were what made them run 44-69% farther; they still differ by the random waits
    they draw. Helmaroc King (BDK 238): M2tower layer 3, reached by its spawn
    points 16, 22 (the arena: `M2tower,22,0,3`) or 30; its other points (0-15, 17...) load to a black
    screen. Converted since: its timers and tick count, its tail feathers (a one-pass chain: their
    stores on whole ticks), pos_move and its states' pos.y += speed.y with a fall or rise (the position
    takes h of the speed, the speed changes `late`, once a tick: exact for both orders, speed first or
    position first), damping d@: mean 2.2 units from 30's path (was 15), tail motion 15.4 against 15.7
    a tick. Not converted: its arena's debris (BDKOBJ 239: fragments with their own physics when broken)
    and 248.
    Phantom Ganon (FGANON 241, GanonC/J/M): absent on the finished save even with WWHD_DEBUG_BOSS=1, and a
    WWHD_DEBUG_SPAWN of it is created and gone; its code's many `+= 1` sites at +0x5BC are mode steps.
  - **The Tower of the Gods** (`tower`; `config/US_v0/tick_rules/tower.txt`): stage `Siren`, each room
    by its own spawn point 0 (`Siren,0,R,-1`; `Siren,P,0,-1` always lands at the entrance), Gohdan in
    `SirenB` (WWHD_DEBUG_BOSS=1). Most of its enemies were converted before (Armos, Chuchus, Bubbles,
    Wizzrobes, Keese, Kargarocs, a Darknut). Converted here: Beamos (Bemos 233: its head's turn, split:
    angles equal to 30's at whole ticks; its beam 232: a count), lifts (Hmlif 40: a count of ticks Link
    stands on it, keep; they run 30's speed exactly, and drift only where Link steps on one at another
    moment), balance lifts (111: a damped spring: its pull vec@, damping d@ on VECScale, its point vec@,
    two QUATSlerps k@ on t: the trial's half steps then equal the tick), Gohdan (BST 240, head and hands:
    WWHD's fields + 0x234; its timers, pos_move's pos += speed, a recoil, its collision push added by hand
    (a push a tick: *h), the hands' slams and rises (explicit order: speed changes `late`): its hands ran
    four times as far before, 5-22% off 30 now; mDamage (+0x130E) is a state step, not ruled), and with
    no rules, matching: statues (Obj_Try 458, 4 units higher at rest), floor switches (Obj_Swflat 29),
    its water (Obj_Tide 39), Obj_Hha 136, Obj_Htetu1 137, stakes (KUI 250), Obj_Hcbh 150. Not converted:
    the light bridges (427, 428: their animation frames drift from 30's), Hys (450), the hot floor
    (231: a Beamos's scorch, its path doubled).
  - **Link's items** (agreed with session top once the queue was done; `config/US_v0/tick_rules/link_items.txt`,
    tested on a new route, `items` (`tools/reference/routes/items-100.txt`: on the Outset dock an arrow, X and
    Y twice each, then the bow with fire, ice and light arrows nocked by turns and a light arrow shot; the
    100% save has the Deku Leaf on X and the grappling hook on Y, `WWHD_DEBUG_POKE` puts others there).
    Arrows (ARROW 472: the move a VECAdd of 200 a tick, `vec@r4`; the fall past 25000 `late`, a bounce's
    gravity, which comes before the move, `whole`; counts): 30's flight exactly (25027 against 25027
    units). The boomerang (BOOMERANG 432: its spin, homing turn `split@` with the bank still seeing the
    tick's turn, its flight, three inline adds): 17.6 apart on average (it was 700). Its blur trail
    shifts a fixed amount a call, so at 60 it is half as long (not ruled: on whole ticks it would trail
    the boomerang by half a tick every other frame). The hookshot (HOOKSHOT 169: the tip's step out,
    back, and Link's pull, `*h@f1` on cXyz::operator*'s scalar): 34 apart (was 188). The grappling hook's
    rope (HIMO2 446: its throw `vec@r4`, five countdowns, a tick count): 12.7 apart (was 96); its rope
    (100 points placed in one pass from each end, no time step) settles faster at 60, not ruled. The
    magic arrows' glow (ARROW_LIGHTEFF 474) and the ice an ice arrow leaves (ARROW_ICEEFF 473, from the
    decomp: no arrow hits anything on the route). Not tested: the boomerang locking onto targets, the
    hookshot pulling Link to a target, swinging on the rope, the bait bag (ESA 221 not converted). The
    Ballad of Gales' cyclone (TORNADO 443: texture frames, spin angles, a timer, a fade) too: in the warp
    (an event, where converted processes don't step) it still runs once a tick, as at 30.
  - **Link walking while he aims, fixed (shared; regress.sh rerun)**: WWHD lets Link walk while he aims
    the grappling hook or the boomerang (the GameCube's stands still). f_02416B70 takes mNormalSpeed toward
    the stick's speed with setNormalSpeedF, and those aims then multiply it by 1.2 (12 x 1.2 = 14.4); at
    30 the next tick's least deceleration step (4) takes 14.4 back to 12 at once. At 60 cLib_addCalc's
    override made that step 2, short of 2.4, and x 1.2 a step ran away: on the items route a short stick
    tap took Link from 12 to 536 a tick and off the dock in 20 ticks. setNormalSpeedF's least and most
    steps are divided by h at f_02416B70's two calls (the override takes h of them again: 30's steps a
    step); Link's path now matches 30 within a few units.
  - **Link gliding with the Deku Leaf, fixed (shared; regress.sh rerun)**: procFanGlide (f_02438E78) adds
    the wind's push (m3730) to his position itself, besides posMove's ruled adds; at 60 he drifted
    sideways twice as far (861 against 349 units in 46 ticks) and landed somewhere else. That add (vec@),
    the glide's turn and its bank's carried turns (split), its countdowns and its magic drain (a timer that
    drains a point at 0: its store, the drain and the reset whole) are ruled; on a new route, `leaf`
    (`tools/reference/routes/leaf-100.txt`: off the Outset dock's end with the leaf opened in the air),
    the glide at 60 keeps within ~10 units of 30's to the water.
  - **Link at 60, what is checked (session bottom, 2026-10-04)**: each by tracking Link at 30 and 60 on a
    route (his position, procedure, speeds): walking and turning (walk, ladder, land), rolling (A while
    running: same speeds 26/25.5/21.9, the rolls end within half a tick), crawling and hanging from a
    ledge and moving along it (crawl), gliding with the Deku Leaf (leaf), swimming, diving and coming up
    (kindan room 11 from session top; the items route), aiming the grappling hook and the boomerang while
    walking (items), his items (arrows, boomerang, hookshot, grappling hook), lifting and carrying a pot
    (carry: a pot spawned with Outset's own small pots' params, 707fff00; one of param 0 doesn't lift).
    Not driven yet: climbing vines, pushing and pulling blocks, hiding in a barrel, swinging on the rope
    (its phases and pump are ruled from the decomp: unruled, the swing ran at twice its frequency; a stake,
    KUI 250, spawned above the Outset dock let the hook be thrown at it with ZL held, but the hook came
    back instead of wrapping), the hookshot's pull, the boomerang's lock-ons. Everywhere Link's camera-relative heading is ~1.5 degrees off 30's (the camera's
    control angle, below), so long routes drift a little (a wall's corner reached later, then apart).
  - **Rope bridges (BRIDGE 89), ruled, not converted** (`earth.txt`): the most placed unconverted type that
    moves (a census of every stage's placements against the defaults: GRASS, tags, switches, static
    models come first; then bridges 32 in 7 stages, fires 424, WARPLIGHT 106, magma 434, Obj_Barrel2 457,
    the Hyoi seagulls 195, Kbota 27, Ykgr 397, which session top took). Its phases, stress count and
    countdowns are ruled; its planks are a chain with no time step. Link never got onto the one tested
    (M_Dai room 9; `WWHD_DEBUG_STAGE` points index the stage's spawn list: M_Dai,12 is point 0x0A), so
    it waits for a test with Link crossing one (DRC's, or one outdoors for the wind's sway).
  - **Open: the follow camera turns ~13% slower at 60 while Link swims and turns** (session bottom, from
    session top's kindan room 11 drop with the walk route): after the water-lift fix Link's dive matches,
    but over a 16-tick turn (f1152-1168) the camera's control angle (camera process +0x2B4, mAngleY:
    `mDirection.U().Inv()`) turns 3109 at 60 against 3583 at 30, and Link's target heading is the
    stick's angle + that (m34E8, Link +0x6930, read from the camera's last frame), so from ~f1190 he
    swims another way. The view's eye-to-centre yaw stays within 0.1 degrees. followCamera's trial
    sites there are all ruled (k@ and k75@ on its approaches, the turn ramp m38C/30 counted on whole
    ticks): the gap is likely the k75 approximation with a factor that changes every tick. In a
    sustained turn it isn't a rate: Link swimming in circles off the Outset dock (stick up-left, ~190
    ticks) has the control angle turning +10496 against +10570 over 48 ticks, ~300 (1.6 degrees) behind
    30's throughout; kindan's 16 ticks were the turn's start. Not changed.
  - **Link's own position adds, audited (session bottom)**: every VECAdd into Link's current.pos and
    every inline `pos += x` in his code (actor_rmw.py 168, 44 sites) is ruled, or adds a per-call
    measure (the animation's root motion since the last call, posMove; the hands' spread change,
    procClimbMoveSide), or a one-shot amount cleared after use (the collision push and m3644, posMove), or
    is a procedure's one-time setup (swim in and out, climb-down start, hang-fall start; setGrabItemPos
    places the carried actor). The glide's wind drift (an approached state added a call) was the one
    runaway. Hanging from a ledge (a new route, `crawl`: crawling off the Outset dock's end, then along
    its edge) went half as far at 60: procHangMove takes mNormalSpeed from the hands' spread change since
    the last call, which posMove then uses as a speed a tick; that change reads per tick now (/h) and the
    shimmy matches 30's path. Crawling itself matched. Not driven by a route yet: climbing, carrying,
    pushing blocks, the barrel.
  - **Joint callbacks run with every drawn frame's calc (a hazard)**: a converted process's model is
    calculated each frame it is drawn (twice a tick), an unconverted one's once a tick, and the step is 1
    there, so step rules don't apply: an increment in a joint callback (J3DNode calc callbacks, e.g.
    the Wind Temple's wall fans' blades, FAN 299: `mFanAngle += mFanSpeed` in nodeCallBack) runs twice a
    tick once the process is converted, and the step-doubling trial (execute only) doesn't see it. 30
    against 60 does (the fans' +0x634 differed on every tick). FAN is left unconverted (wind.txt).
  - **Link's hat and his bob in water (shared; regress.sh rerun)**, from session top's swimming test: the
    hat (f_024022D0) leans away from its move since the last call and flutters by a phase that adds 1500 +
    4060 f a tick (f from the wind and that move's length). At 60 the move was a step's and the phase went
    on twice a tick; with the move read per tick (/h) and the phase's add split, on the walk route the
    flutter's phase advances 3862 a tick (30: 3845; it was 6585), its swing 950 (944; was 716), the
    sideways lean 4765 (4765; was 6066). Also the swim wait's bob phase (0242F5E8, split) and the wait's
    countdown to an idle (02417B84, keep). Under water Link rises by `speed.y += lift` a tick
    (changeSwimUpProc f_02421FBC): at 60 he came back up twice as fast after a drop into deep water
    (session top's kindan room 11 test); *h@ on the add: he surfaces on the same tick as at 30. In
    `link_items.txt`.
  - **Pots and stones** (TSUBO 453, STONE 454; agreed with session top, not in the queue;
    `config/US_v0/tick_rules/carried.txt`). Shared: **daObj::posMoveF_grade** (f_023121C4, also behind
    daObj::posMoveF_stream: the move of what is thrown, rolls or floats: pots, stones, barrels, bombs)
    has a time step now (src/overrides/sixty_step.cpp): its accelerations (gravity, the stream's linear
    and quadratic resistance, the slope's friction, the extra acceleration) are h of themselves for the
    call, and gravity's part is noted for posMove's arc correction. New step op **`ssplit@rN`**
    (generate.py, ppc_ops.h): for a call, the s16 that rN points to is split between the whole and the
    half tick (put back after), for cSAngle's `+=` of an angular speed passed by address (the pots'
    and stones' tumble and spin: `split@r4` on the `-=` of their drag, by value). Pots: their slide,
    push and wall damping (d@), the squash spring on landing (forces *h@, damping k@, scale += v *h@),
    their height in water (k@), their bob's phases, a decoration's bounce (late). Tested by spawning
    one on the Outset dock and poking a throw into it (speeds, mode, spins: see carried.txt): a thrown
    pot 494 against 493 units, 0.3 apart, its spin within 0.2%; a thrown stone 1306 against 1307.
    Not checked: a rolling pot (mode walk) and floating ones (afl), by the decomp only.
  - **`fall@fREG`** (new step rule, shared: generate.py, sixty.cpp's `g_rtActor`, sixty_step.cpp's
    `rt_step_fall`): many executes inline fopAcM_calcSpeed's `speed.y += gravity` instead of calling
    it (a scan for `lfs fG,0x374(rA)` ... `fadds` ... `stfs 0x340(rA)` found 62 sites in ~25 types),
    so the calcSpeed override's time step missed them: at 60 they fell twice as fast. `fall@` on the
    fadds takes h of the gravity and hands it to posMove's arc correction, as the override does.
    Applied to the Armos, Wizzrobes and, in `tick_rules/inlined_gravity.txt`, Chuchus, Peahats, Boko
    Babas and Magtails (Chuchus' regress test: path 517 against 532, was 351). Left for their areas:
    Jalhalla BPW 211 (13 sites), Poes 212, 204 (15), KANBAN 182 (10), Kalle Demos 235, 238, OQ 227,
    NPCs NH 313, NPC_OS 314, NPC_FA1 360, NPC_MD 367, NPC_UK 368, OBJ_HAT 405 (the scan's output is
    easy to redo: the pattern above over .text).
  - Found on the way, not changed: the tevStr's light-change count (+0x1C4 in many actors, from
    `f_025580FC` in settingTevStruct, up to 20 when an actor's light changes) counts steps, so a
    converted actor's light fades in twice as fast; a background helper (`f_024F2514`, d_bg_w.cpp's
    "pupper_pos") writes an actor's x/z a step (seen on Ganondorf and Zelda; not looked into).
- Real-time measuring: `WWHD_FRAME_LOG=path` (every frame's work, GX2DrawDone wait, both threads'
  CPU, vsyncs missed; `tools/sixty/frames.py` summarises), `WWHD_PROFILE` (`tools/profile_report.py`
  on the worker with the same build). On the desktop, `~/wwhd-test` is ours to deploy to and run
  headless; `~/wwhd-play` is the owner's.
- Then D21's order: particles, animation-heavy actors, the HUD, then actors route by route;
  uncapped. Tools: `tools/sixty/track.py` (fields tick by tick, half ticks too),
  `tools/sixty/camera.py` (the camera's view, 30 against 60).
- Run converted: `WWHD_60FPS=1` (the checked conversions are the default now: camera 476, Link 168,
  ship 165, sail 171, seagulls 194, Bokoblins 189 and their sticks 463, Dragon Roost's mountain
  actors 151, 154, 142, 175, 296, 162, push blocks 43, chests 292, doors 300, the sky 437/438,
  Chuchus 206, Keese 215, Moblins 188, Darknuts 191, Kargarocs 181, ReDeads 224, Gohma 234 and
  Valoo's tail in its room 223 (counters, timers, two sways; tested idle in the fight), Magtails 216,
  Peahats 209, Boko Babas 214, Outset's NPC_YM2 316 and NPC_YW1 317, Hyrule's flags 174, the
  capes 192, the Moblins' lanterns 193, Puppet Ganon 243-245, Ganondorf 246, Miniblins 247,
  Bubbles 207, the Wind Temple's fans 114, blade traps 135, Wizzrobes 208, platforms 254 and 252,
  Armos 202/203, Molgera 217/219, the Earth Temple's Stalfos 190, Poes 212, Floormasters 119 and
  Jalhalla 211, the Forsaken Fortress's anchors 431, barrels 456, ropes 447 and Tetra 426, the
  Tower of the Gods' Beamos 233 and beams 232, lifts 40, balance lifts 111, statues 458, floor
  switches 29, its water 39, 136, 137, stakes 250, 150, Gohdan 240, rats 198, Helmaroc King 238, pots
  453, stones 454, and Link's arrows 472, 473, 474, boomerang 432, hookshot 169, grappling hook 446 and
  the Ballad of Gales' cyclone 443 (session bottom), Windfall's 123, 220, 353, 170, 368, 374, 364,
  373, 352, 121, 445, 309 and Outset's 118, 68, 73, 200, 255, 71, 67, 335, 319, 320, 331, 461, 45
  (session top; `kConvertedByDefault` has a line per session),
  the plants, and particles; `WWHD_60FPS_CONVERT=list` replaces the list,
  `WWHD_60FPS_CONVERT=` (empty) converts nothing, `WWHD_60FPS_PARTICLES=0` keeps particles at 30) (with
  `WWHD_60FPS_FROM` for routes). `tools/sixty/run.sh` takes `SIXTY_FRAMES=N` to end a route early;
  the probe writes `particles.txt` (the particle census, every frame); for side-by-side runs copy the
  binary to a folder of its own (`CEMU_BIN=`): one instance per folder.

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
