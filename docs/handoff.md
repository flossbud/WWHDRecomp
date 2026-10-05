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

### Two workers: the owner's desktop first, the worker as the fallback (from 2026-10-05)

- **The desktop worker is the default** while the owner lends it (`desktop`, desktop CPU, 24
  threads, AMD GPU): the same `wwhd-worker` image under rootless podman, with all 24 threads and
  28 GB of 31, `/wwhd` being `~/wwhd-desk` there, with the game, saves, tools, caches, the checks'
  references (gx2 streams, the save/route/tour/sail/menus/warp traces, the g3 captures) and the
  Ghidra project. `tools/worker/sync.sh`, `w`, `job start` and `publish.sh shot` go there by
  themselves while its worker runs (`tools/worker/target.sh`); `job wait/status/stop/tail` find a
  job on either worker. Measured there: a full build 5:21 (the worker 20-30 min), a spawn test 1:20,
  the full `checks.sh` 16 min, all with the worker's results (every check MATCHES, diff 0
  mismatches, captures byte-identical). Sync and build once there (your `.worker-dir` path).
- **the worker is the fallback**: when the owner takes the desktop back (`tools/worker/desktop.sh
  stop`; `status` says on or off), the same commands go to the worker, whose checkout and build may be
  old: sync and build there first. `WWHD_ON=worker` / `WWHD_ON=desktop` force one. Only main (or
  the session the owner asks) runs `desktop.sh stop`/`start`. While it's on, a user unit
  (`wwhd-awake`) keeps the desktop from sleeping.
- **Three sessions at once on the desktop** (tested 2026-10-05: three full builds from scratch
  together, 13.5 min each, peak 9.8 GB of the worker's 28 GB, with the sessions' jobs running): builds
  default to 8 parallel compiles there (`/wwhd/data/m6/.build.jobs`; `JOBS=` overrides; the worker 10),
  so three builds fill the 24 threads; the process limit is 16384.
- **Check slots per worker**: `tools/sixty/tests/checks.sh` waits for a free slot (the desktop takes 3
  runs at once, the worker 1: `/wwhd/data/m6/.checks.slots`) and prints whose runs hold them each
  minute. A check run can cover several commits.
- **Disk**: `tools/worker/cleanup.sh` (cron on the worker, a systemd user timer on the desktop, every
  6 h) deletes check outputs in `/wwhd/data/gx2/<run>/` untouched for 12 h and test outputs in
  `/wwhd/data/m6/*/<dir>/` untouched for 48 h; references, traces, captures and saves never. Write
  findings down before then.

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
    `WWHD_DEBUG_SPAWN=tick:process[/subtype],param,x,y,z[,anglex[,angley]][;...]` (subtype: the name table's argument, e.g. the sea's Octorok `Oqw` is 227/1) creates an actor there in
    Link's room (the creation record `f_025D5678`, then fpcM_Create `f_025E14A8` on the layer at
    *0x101F3AE8), its angle's x and y in hex if given (x is more parameters for some, y its heading):
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
  - **Round 2, objects (session top)**, in objects.txt and the defaults:
    - the Tower of the Gods' light bridges and stairs (LIGHTBRIDGE 427, LIGHTSTAIR 428): their animations
      and fades are stepped helpers; their events' orders `late`, the stair's appear countdown `late`.
      Trials clean (Siren rooms 2, 4, 12, 16); the bridges equal 30's; the stairs' frames equal 30's but
      lose half a frame at each event's start: a converted actor's tick there is split (its whole-tick
      step runs, the event begins, its half step is cancelled). That's the shared event gating (sixty.cpp,
      `f_025DE58C`); session top takes it after session qa's event-reset change;
    - the hot floor (Hot_Floor 231, a Beamos beam's scorch): no rules, it follows the beam's hit point
      and fades on stepped timers. With a Beamos spawned facing Link on the Outset dock its trial is
      clean; its path and life follow the beam, which fires about 4 ticks earlier at 60 (session bottom's
      Beam/Bemos, noted to it);
    - eye switches (Hys 450): the eye's frame ±1 a tick `keep`; poked into its wait in Siren room 1 it
      closes over the same three ticks as at 30;
    - mailboxes (OBJ_TORIPOST 67): no rules; equal to 30's on routes save and tour;
    - item stands (STANDITEM 462): four animation countdowns `keep` (one at 0 starts the other, so a half
      step never acts twice), a wind sway's strength `*h@` and phase `split`; Windfall's fourteen: trial
      clean (their idle timers come from the random stream at creation). Some carry a cloth: the chains
      item;
    - bomb flowers' bombs (BOMB2 295; session qa's B17: they animated at 30 beside Link's bombs): the fuse,
      the explosion's collider time and the sink's count `late`; its wobble a damped spring; the fuse
      smoke's three-point trail shifts once a tick (`whole`) while its tip moves every step (the smoke's
      particle speeds come from the points' per-tick differences). Tested with session qa's setup: route
      `carry` with `WWHD_DEBUG_SPAWN=950:296,0000ff00,-201660,190,312465` (a bomb flower where the pot
      was; Link pulls the bomb at f1000).
    - Dragon Roost Cavern's steam vents (SteamTag 423), flame lifts (MFLFT 91) and swinging platforms (MSW
      90), session qa's B13 ("the big lava geysers and the platforms they lift look 30 fps"): the vents'
      on/off countdowns `keep`; the lift and the platforms sway by a tick count, sin(count x K): the count
      `keep` and each phase a new step rule, `lagi` (shared, `generate.py`: on a `mulli` of a tick count,
      the whole tick's step lags by IMM/2, so the phase is (count - 1/2) x IMM there and count x IMM at
      the half tick: the sway moves every frame and is 30's at half ticks); the lift's fall `*h@`/`late`.
      M_NewD2 room 2: trials clean, the lift's path 5375 against 5435 units (6930 converted without
      rules); room 4's twelve vents: trial clean (their timers are random).
    - Link's bombs (BOMB 294) held until they blow up didn't knock Link back at 60 (session bottom's find
      while testing B19): the bomb registers its collider once per g_Counter.mCounter0 (`mMassCounter`),
      which steps once a tick in the whole tick's draw, so a half step and the next whole step share a
      value; the half step's registration stamped it and the blast on the next whole step skipped its
      200-unit attack sphere. The stamps `whole` (sea.txt); the half step's Set and SetMass were dropped by
      their overrides anyway. Now Link takes the hit (proc 0x68 from f1155.5, landing 0x69 at f1164;
      30: f1156 and f1165, as hits resolve on whole ticks). Only the bombs guard on that counter (three
      NPCs use it as a random bit).
    - The half tick a converted process lost at each event's edge (shared, `sixty.cpp` `f_025DE58C`, agreed
      with session qa after its event-reset change): when an event began, was ordered or was asked to end
      during a whole tick, after a process had taken its whole-tick step, every converted process's half
      step was cancelled, so each moved half a tick less at every event's start (the Tower of the Gods'
      light stairs fell half a frame behind 30's at each). Now only the event's own stop: Link, the camera,
      an actor with its event command set (+0xF8) or the staff status FORCEMOVE (0x8000 of +0x2E0), and the
      actors of a pending order (dEvt_control_c's orders at g_dComIfG_gameInfo +0x51D0, 0x18 each, actors
      at +0x08/+0x0C, count +0xC0, checked against WWHD's order function f_0253EC0C); the rest finish their
      tick (`InEvent`; `WWHD_60FPS_EVENTEDGE=0` as before). Siren room 2's stairs now equal 30's at every
      tick (they lost a frame by f1400 before); session qa's door routes `back` and `door2` end as before
      (the same doors at action 1, Link within 5 units of 30's). fopAc_Execute now always notes actors
      (`s_knownActors`, which only the convert-all probe filled).
    - Not yet: door knobs (KNOB00 305: a door whose motion is its open event, which steps at 30; left for
      the events item and session qa's door fixes), Windfall's night lights (Obj_Light 128: its decomp is
      stubs, and its phases run at night only) and shutters (SHUTTER 259: they move in their open/close
      events). In daytime on Windfall both match 30 at every tick unconverted. A night test is still to
      do: poking 330.0 (22:00) at g_dComIfG_gameInfo + 0x24 (where the GameCube's dSv_player_status_b_c::
      mTime would be) before the warp changed nothing (both still matched, as by day), so WWHD's time of
      day lives elsewhere.
    - The early BoatBattle camera on route `sail` (session bottom's find: f1293 at 60 against f1338): the
      Fishman (NPC_SO 118) asks for it from its near swim, which it enters from its swim once Link on the
      boat is within its radius. At 30 it was mid-jump then (back in the water at f1336); at 60 its random
      jump timer (rnd(90) + 30 ticks) drew otherwise and it was swimming. The random stream, not a step.
    - B10 (Beedle's ships sail too fast; session qa handed it over): not reproduced. Outset's Beedle ship
      tracked on route sail: speed 12.00/12.00 a tick, path 16132/16153 units over f900-2900, its bob
      phase and height identical tick for tick.
  - **Round 2, rito (session top; `config/US_v0/tick_rules/rito.txt`)**: the Rito Aerie (stage Atorizk) and
    the mountain path (Adanmae, most of it session bottom's from round 1). Converted: post boxes
    (Obj_Ospbox 84: no rules, equal to 30's), the Rito (NPC_BM1-5, 326-330: their blink's frame `keep`),
    the mail sorter (NPC_BMSW 348) and Komali (NPC_ZK1 371) (blinks, `keep`), Valoo (DR 222: three
    countdowns `keep`). Trials clean (Atorizk,0,0,-1 and Adanmae,0,0,-1, f1000-1500); the walking Rito's
    paths equal 30's (1996/1996, 1999/1998 units); blinks, idle choices and Valoo's moods differ from 30's
    in phase (their random waits draw differently at 60). Not present on the 100% save there, so not
    converted: Medli (NPC_MD 367), Obj_Rcloud (141), Obj_Eskban (160); the chieftain (NPC_BM1) shares the
    Rito's code and rule.
  - **Round 2, islands (session top; `config/US_v0/tick_rules/islands.txt`)**: a census of the islands' and
    caves' stages (Fairy01-06, MiniKaz, MiniHyo, Cave01-11, PShip, Abship, ShipD, Pjavdou, Ojhous, Ocrogh,
    Otkura, Omori) for unconverted placed types; most are switches and tags. Converted: the Great Fairy
    (BIGELF 369: a count to 255 `keep`; now equal to 30's), Fire Mountain's magma rocks (42: explicit
    Euler, `*h@` on pos.y += speed.y and `late` on every speed store; their tilt's slerp `k@`; trial clean,
    they bob out of phase as the lava's random bubbles under them differ), Cave03's Kryu00 (33: three
    phases `spliti`, a damped spring, a height history `whole`; paths within ~10% of 30's, 2.4x before),
    the submarines' rat holes (199: countdowns `keep`), and with no rules, trials clean: Jabun's cave's
    water (143), Orca's house's plants and papers (274, 262), Sturgeon (NPC_AJ1 332), the Savage
    Labyrinth's traps (287), Obj_Akabe (87), Ice Ring Isle's ice (460), tables (64). Orca (NPC_JI1 318, converted
    later: islands.txt): his wobble's phase (its step made from how far a vector moved: /h on the length,
    then split) and a count stepped while a countdown is out (on a half step the helper still says "out":
    `whole:r3=1` on the call); his look-at needs nothing (it adds only its steps' own changes); the phase
    keeps 30's rate. A Korok in Ocrogh (NPC_BJ7 342: trial clean, within 3.8 units of 30's) and Tingle's
    tower's Tpota (394: its waterfall's ripples, a table mirroring the particles, which move every frame at
    60) are converted with no rules. Not present on the 100% save:
    Forest Haven's Koroks (NPC_BJ1-9) and Makar (NPC_CB1), Jabun (NPC_JB1 358); Cave08's propellers,
    fans and nets never loaded at the spawn points tried (point 0 of rooms 1-3: they need the stage's
    spawn list).
  - **Round 2, sealife (session top; sea.txt, forest.txt)**: the sea creatures in action, spawned in the
    sail route's way (`route_test.sh sail TYPE`; `pair.py`-style pairing by type, since a spawned actor's
    heap address differs between the rates):
    - Seahats (PH 209, `1450:209,ffffff01,-213500,400,310200`): their attack cycle (climb, hover, swoop) ran
      every 60 ticks against 120: five countdowns (+0x480, a loop in the shared execute; land Peahats'
      too) and the hover's countdown (+0x48E) `keep`. The hover's height is 900 + b (b the floaty bob,
      30 sin): a snap to 900 (scale 1, cap 50) then b added through an offset eased with a cap of 30, so b
      is a displacement where the snap holds it and a speed where it climbs (a tick adds 50 + b). At h the
      snap aims at 900 + (1 - h) b and a step adds h b (the new rule `reloadh:`, below, with `*h@`, and
      `/h@` undoing the step override's halved caps): the hover now equals 30's (919.4, 928.0, 929.7...),
      the cycle is on 30's ticks, the path within 0.1% (before: clipped flat at 915, the cycle twice as
      fast). They stay ~400 apart after the swoop's bounce off the boat (a collision).
    - Land Peahats (kindan room 9): a wobble phase (+0x478, fly_angle_set) `spliti`; trial clean.
    - Octoroks: the sea's ("Oqw" is 227 with subtype 1; `WWHD_DEBUG_SPAWN` takes `proc/subtype` now:
      `1450:227/1,ffffff01,-212500,0,311000`) surface, wait out a countdown and jump at the boat: six
      countdowns (+0x3E6, a loop) ran two a tick (`keep`), the jump's four inlined calcSpeed gravity adds
      `fall@` (it peaked at 122 instead of 246), and the bob's phase on the waves (`spliti`, the pools'
      was ruled). Now the same modes on the same ticks, the jump within half a tick. The pools' (kindan
      room 12) countdowns now count per tick too; they part with random waits.
    - Big Octo and Gyorgs: as round 1 found (trial clean but for copies; retested).
    - Not tested: the Hyoi seagull's flight (NPC_KAM 195, `HyoiKam` over each island: a Hyoi Pear used
      within its range, `scale.x` around its perch, and its talk distance (38) starts its descent;
      `WWHD_DEBUG_POKE=990:0,1046F12E,1,83;990:0,1046F154,1,5;990:0,1046F0B9,1,24;990:0,10474C6B,1,83` puts
      a Hyoi Pear on X (bait slot 0, gameInfo +0x7E/+0xA4 as on the GameCube), but on the Outset dock the
      perch is 8000 away and WWHD_DEBUG_PLACE under it didn't move Link). The perches are 2000 units above
      small islands' spawn points (`stage_actors.py sea ROOM --spawns`; room 19's point 1 is 83 units from
      its perch, a cave's exit): the pear raised there (X at f1150, after the exit) called no seagull in 200
      ticks and the seagull's process never moved; its range likely wants Link on the island's top. Left.
    - Shared: `reloadh:fD=rB+O` (generate.py), `reload:` in a stepped process's steps only (`reload:`
      applies at 60 fps even to a process that isn't stepping); `WWHD_DEBUG_SPAWN=tick:proc/subtype,...`.
  - **Chains drawn at 60 (the chains item, session top; shared: sixty.cpp)**: one-pass chain solvers keep
    the 30 Hz step (rules `whole` on them), so their points stood still on half ticks while the actor they
    hang on moved. Those drawn as 3D lines are now drawn half a tick on along their last tick's motion on a
    half tick's frame (the points as the whole tick left them plus half of what that tick moved them),
    only while the line's vertices are built (mDoExt_3DlineMat1_c::update f_025EA548 and the other line
    class's f_025EC62C; the solver's points are put back), only for a converted owner that stepped this
    half tick and only if the points are still the whole tick's. Opted in by process name
    (ChainLines): the Stalfos' hair (190, Earth Temple room 14: `M_Dai,15,0,-1` with
    `WWHD_DEBUG_POKE=980:190,3e0,2,1`) and the rats' tails (198, `Siren,0,6,-1`): their tips now make 46%
    and 47% of their motion on half frames (0% before). `WWHD_60FPS_CHAINS=0` turns it off;
    `WWHD_60FPS_CHAINS_LOG=path` logs each smoothed chain's tip per frame. Model chains the same way, in the
    actor's draw (the fpcM_Draw override: arrays by process name, kChainArrays, moved on point by point,
    a root that follows its actor every step left as it is; angle triplets by half their wrapped change):
    Helmaroc King's tail feathers (BDK 238, four tails at +0x414, 0x17C each, places +0x24 and angles +0x9C,
    tail_draw; `M2tower,22,0,3` with WWHD_DEBUG_BOSS=1): 38% of a feather's motion on half frames (16% of
    them still: the actor held in its events). A Kargaroc's tail (BB 181: tail_control f_0205D4BC, a one-pass
    chain with a velocity, now `whole`; places +0xC1C, angles +0xC94; session bottom's trial): spawned on
    the dock as spawn_test.sh does, its middle relative to the body within 2.8 units of 30's (36.4 when it
    stepped every frame), half its drawn motion on half frames. (The Boko Baba's eyePos, flagged in the same
    trials at x1.6, needs nothing: its execute copies the head's place (+0x3FC, +0x444) before its model's
    calc (draw_SUB, at its end) sets it, so the copy lags a step, a tick at 30 and half a tick at 60, and a
    double step shrinks the lag; its head moves as far a tick at 30 and 60. A copy made before the step's
    own update reads as a doubling in the trial.) The pirate ship's cloths (objects.txt; the ship beside
    Outset's dock with `sea,0,44,2`), converted: its sail (SAIL 172; the boat's sail is GRID 171, converted
    before) is made afresh each tick from a wave phase, now split: its vertices equal 30's at every half
    tick; its flag (PIRATE_FLAG 173) is a mass-spring cloth like Hyrule's flags (vec@/d@ on PSVECAdd and
    PSVECScale): its points move 14.9 units a tick against 30's 14.6; both packets' random shading wobble
    (setCorrectNrmAngle) on whole ticks, so the random stream is 30's. Not done: tapestries (Obj_Tapestry
    289, the Earth Temple's curtains in rooms 2, 5, 10 and 13; `M_Dai,22,2,-1` puts Link among room 2's
    four): unconverted they wave on whole ticks; converted as they are, twice as fast (the trial: the eye
    point and vectors made from the cloth, x2). The cloth is a packet stepped by f_0239C574 through eight
    helpers (per point accelerations: springs, gravity, a wave, hits, in f_0239B71C's six; speeds, a
    position correction, normals in f_0239B85C; wind, hits, fire) and the decomp has their names only, so
    its rules need the code read. The Stalfos' loincloth (its pose set in the execute, nun_pos_set: its angle steps
    on whole ticks, its anchor every step). Other users of the two line classes (ropes, ships' lines,
    vines) can be opted in once tested; the rope bridges' ropes step with their bridge, now converted.
  - **Rope bridges (BRIDGE 89) in the defaults (session top; earth.txt)**: Link walked across two at 30
    and 60, from spawn points facing them with the stick held up: Dragon Roost Cavern room 2's top bridge
    (`M_NewD2,8,2,-1`) and Outset's cliff bridge in the wind (`sea,8,44,0`: layers 0 and 2 have it, -1's
    hasn't). The planks track 30's within a mean of 2.0-2.3 units (at most ~20 in the dip under Link: the
    planks' chain, two passes a tick at 60, follows his weight a little sooner) and make half their motion
    on half ticks; Link's height on them is a mean 5 and 1.4 units from 30's. Unconverted, the planks and
    their collision moved on whole ticks under a Link who steps every frame. On the way: a stage warp's
    point is the low byte of its spawn entry's (PLYR) angle z, a point the room hasn't gives its first
    entry (`stage_actors.py STAGE ROOM --spawns` lists them); `WWHD_DEBUG_PLACE` in play moves Link along a
    line from where he was and stops at the first wall (it left him over Dragon Roost's lava), and a
    `WWHD_DEBUG_POKE` of his facing didn't hold there; capture.sh's extra input lines are game frames (its
    header said swaps; its shots are swaps). events_test.sh's back and talk routes now start from the
    warps their headers name (before, they ran from Outset's dock without their door and talk).
  - **NPCs (the npcs item, session top; windfall.txt, islands.txt)**: a stage survey (every stage's placements
    against the defaults, by `stage_actors.py`'s functions) listed the NPCs still unconverted where players go.
    Converted: Windfall's houses (the auction house's KP1 355, GP1 357, KF1 359; Kaisen's KG1 362; Lenzo, PHOTO
    375; the school's HO 366; the pirate ship's hold, Obombh: P1 322 and BMS1 347), the islands' (stalls ROTEN
    372, HR 365, the Koroks planting on islands BJ5/6/9 340/341/344, the fairies FA1 360, AC1 376, the Tingle
    brothers TC 325, KG2 363, AH 381, BMCON1 346; the other Koroks planting on islands, BJ1-4 336-339 and BJ8
    343; the boating course's SARACE 324, sea,100,48,-1), Beedle's shopkeeper BS1 345 (Obshop) and DS1 351
    (Pdrgsh).
    Nearly all blink the same way: a countdown (cLib_calcTimer<s16> f_02055B64, `keep` in tick_rules.txt) that,
    once out, steps the blink's frame until its length, then draws a new wait; on a half step the helper still
    returns 0 and the frame stepped twice a tick: `whole:r3=1` on that call (r3 = 1: not out; Orca's count is
    the same pattern; `whole` on the frame's store alone breaks the reset, which compares the register). A few
    count a byte to 255 (late) or a countdown tested at once (keep). Tested by area_test.sh in their rooms
    (`STAGE,point,room,-1`, walk), trials clean after. Not running on this save by day, so left: the auction's
    bidders (AUCTION 361, 382), Carlov's MT/MN 379/380, CO1 370, HI1 377, Outset's YM1 315, LS1 321, P2 323,
    BTSW2 350. Medli and Makar: session bottom left both unconverted (on this save they only stand in their
    boss rooms; their companion behaviour needs a save from mid-game). The trials only see the blinks their
    walk reaches, and many NPCs blink in a mode it never starts (talking, an event): `tools/sixty/blink_audit.py`
    (on the worker) lists each converted process's helper calls whose 0 steps a field + 1 with no rule; it found
    Outset's YW1 317, Sturgeon AJ1 332, AC1 376, KF1 359 (a blink, and a count up in what looks like the
    auction's bidding) and Zelda PZ 210 (modes 0/7 count down, 3/4/8+ step the frame with no countdown), now
    `keep` on the frame's add, and with a window of 64 instructions (some blinks test the frame's end
    before the + 1, as SARACE's, which the trial caught) BMS1 347 and DS1 351 (`whole:r3=1`). What it still
    lists is no blink: NPC_SO 118's state number (+0xCEC, + 1 once
    a countdown is out: phases 1 and 5 go on to 2 and 6, at most half a tick early), process 198's random
    wait (rand & 1 + 1) and Gyorg's +0xA44 (behind its tuning data's +0x99, set at run time). With -a, the
    unconverted NPCs above blink the same way (LS1, PF1 356, CO1, HI1, MT, MN, BTSW 349, BTSW2, AUCTION 361
    and Medli MD 367): give them the rule when converting.
  - **Outdoors (the outdoors item, session top; windfall.txt, sea.txt)**: the moving types still at 30
    outdoors, from a census of every stage's placements against the defaults (session top's objsurvey.py on
    the worker: GRASS 435 spawns the scene's plants, already at 60; tags, switches and static models lead
    the list). Converted, each with area_test.sh in its place: the cafe's and school's lamps (Obj_Cafelmp 280:
    a spin, `spliti`), the auction house's flowers (Obj_Rflw 277: a swing's count `keep`, its phase
    `spliti`), the boating course's goal flag (Goal_Flag 291: the pirate flag's cloth rules, `split@` on its
    three phases, `vec@`/`d@` on its points; RaceEnd's count `keep`; its race timer is the timer process's),
    the sea's barrel spawner (Coming3 271: a countdown `keep`; eight of ten follow 30's paths within a few
    units, two part where their barrels sink and come back), Ice Ring Isle's lifts (ICE_LIFT 93: counts
    `whole`; the bob reads (count << 10) & 0xFC00, so it stays at 30 Hz; the moving one runs 8.6 units from
    30's on average, its states changing on a step's exact `==`), dragon head (Obj_Dragonhead 56: its fade
    `spliti`) and ice (Obj_Iceisland 55: its wind's volume ramp `whole`), Tingle Island's Obj_Vtil 459 and
    Beedle's shop curtain (Obj_Bscurtain 152; trials clean). Their fields that still differ from 30's are
    lights their draws set, values set at their creation (after the warp, so from another random stream or
    sea), or the sea's height under them. Left at 30: Fire Mountain's volcano (51: idle on this save; a
    count in a state it never entered) and Obj_Yboil 276 (didn't run), liftable rocks (Stone2 455) and
    ladders (Obj_Ladder 85: knocked down in an event; on this save they lie fallen), Outset's gong (284) and
    the Flight Control Platform's goal flag (didn't run). Later: the islands' trees (Obj_Ftree 131: the
    sway's count `late`, its phase `spliti`; its amplitudes, sin(phase) before the add, show a half step's
    sampling in the trial, x1.6). Left: the Triangle Islands' statues (Obj_Doguu 265: a glow drawn at random
    each step, as torches), AYUSH 178 and Obj_Rcloud 141 (didn't run in rooms 23, 34, 13).
  - **Night (session top; sixty.cpp's `WWHD_DEBUG_TIME=tick:degrees`, windfall.txt)**: WWHD's day clock is
    the float at +0x44 of the save info the pointer at 0x101F84DC holds (dKy_getdaytime_hour f_02556C34
    divides it by 15; 1490 functions load that pointer; in this run it was 0x145B7BA4, on the heap). The
    game info at 0x1046F0B0 has a copy at +0x24 (the save file's place for it) that the day doesn't read:
    poking it, before or after the warp, changed nothing. `WWHD_DEBUG_TIME=905:345` (23:00, before the
    f920 warp) gives a night Windfall. Converted: the night lights (Obj_Light 128: spin and flicker phases
    `spliti`, its event counts `keep`). Run by night but left: the auction's bidders (NPC_AUCTION 361) and
    the auction (AUCTION 382), idle clean but for the bidders' blink; their bidding counts its times in
    the auction's event. Not running at 23:00: the shutters (259), NPC_PF1 (356), 197. Owner bug B16
    ("long blue streaks across the screen" in Windfall, never reproduced by day): session qa's streak.py
    over 600 consecutive swaps at night (and 120 at the town centre) finds no one-frame glitch; the night
    sky's shooting stars (HD-only) draw long thin trails, as long at 30 (`CAPTURE_30=1`) as at 60.
  - **Link's action timing (session top; shared: generate.py's `hold`, sixty.cpp's ActionHold; link_actions.txt)**:
    at 30 a tick makes one call to his action ((this->*mCurProcFunc)() in daPy_lk_c::execute, the pointer at
    +0x61AC, two bctrl's: 0240D6D8, 0240D6F8; his action number at +0x65F0); an action that ends calls the next
    one's set-up, and for some the new action's own step only comes the next tick. At 60 a set-up made in the
    whole step ran that action's step in the half step too, half a tick ahead. Route door2 (session bottom's
    find: Link 25 units ahead at the door): his turn in place (0x17) started turning in f1000's half step while
    30's tick only set it up, ended a tick early, and waiting (4) and the walk (6) followed, 1.5 ticks early.
    Fixed for set-ups of the turn in place and of waiting: `hold` rules on the call (the new kind: skipped
    while g_rtHold is up, its entry and exit noted by rt_hold_enter/leave in sixty.cpp) and g_rtHold raised for
    Link's half step after a whole step whose call set one of them up; the rest of that half step (the common
    move, collision, animation) runs. door2: every action on 30's tick, Link within 5.3 units (mean 2.2; was
    17.1, max 25.6). `WWHD_60FPS_ACTIONHOLD=0` turns it off. Tried and dropped: holding the whole half step
    after any change (it lost half of a new action's first move: 6.3 behind on door2); leaving the call out
    after any set-up made inside it (door2 1.7, but a walk from standing, 4 to 6 on Outset's dock, ended 6
    units behind: the common code after the call changes his speed too, 0.36 in the held half step, and his
    animation ran ahead of the move's logic; and a roll set up from a roll, 36 to 55, lost a step: at 30 that
    one's own step came in the same tick). Set-ups made before the call (a roll on A, from checkItemAction and
    the like) step in the same tick at 30 too and need nothing. A general rule would need to know, per action,
    whether its set-up's tick at 30 includes its step; the walk from standing and rolls stay half a tick early.
  - **`splitd@REG` (shared: generate.py; the Morth's spin, tick_rules.txt)**: `split@` while the process
    steps, and the instruction doesn't run on a half tick's draw (a half tick with h 1: only stepping
    processes run then). The Morth's draw_SUB (f_021A6F64, its body's spin m2FA += m2FE) runs from its
    execute, which sets m2CD, and from its draw when m2CD is 0 (no execute since the last draw). A half
    tick's draws are put back afterwards (WWHD_60FPS_ROLLBACK) for every process that executed on the
    whole tick, unless it's converted and stepping (whose half-tick execute set m2CD), so only a process
    the manager doesn't execute but still draws was left: at the pause menu's first frames the spin moved
    2200 at 60 against 30's 1100 (session bottom's audit of rules a draw reaches); now 1100.
  - **A tick that began stepping in an event finishes its half step (shared: sixty.cpp, 2026-10-05)**: the
    half tick's stop for a running event read Link's action again (StepInEvents), so where it left the
    list during the whole tick's step every process that had begun stepping lost the tick's other half:
    the Wind's Requiem's change of wind (0xC4) handing over to turning to wait (0x17) at f1423 left Link's
    turn half done (8186 -> 12186 against 30's 16186), the event ended a tick late. Each process's
    stepping in a running event is now decided once a tick at its whole step (s_stepInEventAtWhole), the
    half tick using that; the turn and the event's end are on 30's ticks. The routes back, door2, talk,
    cuts, items, warp and door are unchanged by it. The camera's view after the song (the view saved at
    the song's start against behind Link) is the camera's own stepping in the song: with it unconverted
    the view equals 30's (session bottom has it).
  - **Countdowns in session top's types (the counter audit, 2026-10-05)**: session qa's
    `tools/sixty/counter_audit.py` lists every stored countdown with no rule, the ones a trial sees only if
    its test runs them. Of the 94 in session top's types (and the boat, Kargarocs, Keese, Gohma), ~70 are
    ruled in their area files (and tick_rules.txt for the base types): `keep` on the add where the count is
    tested right after it (a record-form `extsh.`/`rlwinm.` and a branch, or a compare: the half tick
    neither counts nor acts), `late` on the store where it's tested elsewhere or before (once a tick, at
    its end: exact either way). Notable: dropped items' lifetime (item 255, mSimpleExistTimer: they
    vanished at half the time), AND_SW0's timed switches (307), the townsfolk's (373) and Killer Bees'
    (368) waits, the mail sorter's (348), Big Octo's, the warship's, the magma rock's state wait (its old
    value tested: `late`, the state change resets it). Not ruled: damage (`+0x3A1 - 1` after a hit's
    sound: the sea cannon, warship, Big Octo's eyes, pigs, Gohma), counts changed on events (the Morths'
    stuck count KUTTUKU_ALL_COUNT, a bomb's release, the ship's cannon using a bomb, the mail sorting's
    presses, an item stand's sale), a loop's set-then-count (the Great Fairy's particles), and five in
    shared enemy code the audit gives to VigaH (297: before its own code; for the enemies item). The
    166 counts up weren't reviewed (most are state steps, `+= 1` to the next mode, which want no rule).
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
    Link's hits). Her larvae (BWDS 218, session top, wind.txt: made only when her tongue is hit; spawned on
    the empty arena with `kazeB,1,0,-1` and `WWHD_DEBUG_SPAWN=1000:218,23,0,299,200`): pos += speed by hand
    in seven places (`vec@`), two hand-made falls, a spin, countdowns, and a 14-segment body placed in one
    pass from the head (its drag `*h`, its carried velocity's 0.85 `d@`): converted it went 10951 units
    against 6617, now 5898 (its random waits and turns part from 30's), the body keeping 30's shape while
    the heads coincide. Princess Zelda (PZ 210, the companion in Ganondorf's fight; route gtower with
    WWHD_DEBUG_BOSS=1, the fight from ~f8200; her decomp is stubs) is converted with no rules: the trial over
    f8300-11900 shows only her eye point's copy of a joint (+0x834, derived), the wall push-out
    (dBgW::positionWallCorrect f_024F2514: geometric) and the shared light blend's bytes (+0x1AC); she walks
    at 30's speeds (10.9 a tick moving against 14.2), and the fight itself parts from 30's (she moved in 372
    ticks against 85). Floormasters (FM 119: rules here, converted with the Earth Temple: one matches 30
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
    companion Medli to test; on it she only stands in Jalhalla's room, M_DaiB, where the trial shows her
    hair, a one-pass chain in a joint callback during the model's calc (f_02283A78: joints' places at
    +0x4290, directions +0x42F0, lengths +0x4380), stepped twice a tick: it writes joint matrices, so the
    chains item's draw-time arrays don't fit it as they are; Makar likewise stands only in Molgera's room,
    kazeB, his trial idle clean; both need a save where they are companions), torches (EP 185: a random flicker target each step, as session top found
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
  - **Bug B2, the grappling hook's swing blacked the screen at 60, fixed (shared; regress.sh rerun)**:
    hungCamera (f_0250EF98, work area 'HUNG'), like the shield camera, sets up N and a weight W on its
    first tick (m11C 0) and then approaches by (N - m11C) / W while W loses N - m11C a call. At 60 W lost
    it on both frames, reached 0, and the factor's division made eye, centre and fovy NaN for good (a
    stake swing off the Outset dock: NaN from f1119.5; the HUD drew on black). As the shield camera's:
    the first tick's set-up whole (m11C counts late, so it ran on both frames), k: on the factor, W's
    loss late. After: the swing camera within ~20 units of 30's and the swing on screen (captures).
    The other camera modes without rules (a survey of Run's engine table by their work tags):
    talktoCamera 02508A60 ('TALK'), towerCamera 0250B30C ('TOWR'), rideCamera 0250D4E8 ('RIDE': sailing,
    now ruled, below), crawlCamera 02511200 ('CRWL'), hookshotCamera 02511B5C ('HOOK'), tornadoCamera
    025121E8 ('TRND'), vomitCamera 02513518 ('VMIT'). Hookshot targets (dzb PolyInfo word 3 & 0x10, a
    scratch scan of the rooms' embedded collision): sea rooms 1, 10, 11 (Windfall, 54 triangles), 12, 17, 48;
    none on Outset (room 44).
  - **The sailing camera ran its approaches twice a tick at 60, fixed (shared; regress.sh rerun)**:
    rideCamera (f_0250D4E8, 'RIDE'; the decomp has a stub, ruled from its asm and Ghidra's output). On
    the sail route at 60 it boarded 133 degrees off and, in a turn, pulled back to 977 units against
    402 (pitch -18.5 against -0.7, fovy 60 against 76); in straight sailing it matched. 29 rules
    (tick_rules.txt): k@ on its smoothed boat values, lean, follow factor and point, radius, latitude,
    yaw, fovy and bank; k: on its two entry transitions' 1 / (N - m11C). After: boarding's tail (f1100-1160)
    and straight sailing equal 30's (distance 700/690, 750/749, 557/557; fovy equal). Open: boarding's
    first ticks (f1047: yaw 6 against -128; converges by f1100): the set-up (m11C 0) puts the eye on one
    side of the boat or the other by an angle's comparison (work +0x38 against the boat's heading), and
    at 60 it chose the other side (the eye 480 units from 30's at f1045, the centre equal); likely Link's
    slightly different heading after his swim (the camera lag below), not a rate. The turn's pull-back
    is the BoatBattle camera
    type (38) switched 44 ticks early at 60 (f1293 against f1338): requested by name only by the Gyorg
    controller (GY_CTRL 229/230, f_0216BBBC) and NPC_SO (f_022E06A4), so the Gyorg attack's trigger fires
    early (told session top, sea life). The camera's type is at camera +0x248+0x51C, its style +0x514.
  - **The debug menu (the owner's idea; round 2 debugmenu, session bottom)**: click both sticks at once
    and a panel opens over the game (src/os/debug_menu.cpp; drawn by src/frontend/overlay.cpp as the
    system's dialogs are, so never in captures): Islands, Dungeons, Bosses (refights on), Spawn an
    enemy, "Boss refights: ON/OFF", Close. The D-pad or the left stick moves, A chooses, B goes back (or closes);
    while it is open, and until the buttons that closed it are let go, the game reads no input. A
    destination asks for the stage change on the next game frame, as WWHD_DEBUG_STAGE does
    (wwhd::debug::RequestStage in sixty.cpp); a boss turns the refights on first (WWHD_DEBUG_BOSS's
    switch, now wwhd::debug::SetBossRefight; the env var still sets it at start). The menu filters
    the pad in KPADReadEx (src/os/input.cpp), so an input script drives it too: the scratch route
    `LCLICK+RCLICK 5` at f1000, `DOWN 3` at 1015 and 1025, `A 3` at 1035 and 1045 warps the save route
    to Gohma (M_DragB), and Gohma (234) is there from f1093. Destinations (each warped to on the save
    route; Link arrives in all): Outset (sea,0,44), Windfall (sea,0,11), Dragon Roost Island (Adanmae),
    the Rito Aerie (Atorizk), Forest Haven (Omori); Dragon Roost Cavern (M_NewD2), the Forbidden Woods
    (kindan), the Tower of the Gods (Siren), the Forsaken Fortress (MajyuE), the Earth and Wind Temples
    (M_Dai, kaze), Hyrule (Hyrule), Ganon's Tower (GanonA); Gohma (M_DragB), Kalle Demos (kinBOSS),
    Gohdan (SirenB), Helmaroc King (M2tower,22,0,3), Jalhalla (M_DaiB), Molgera (kazeB), Puppet Ganon
    (GanonK), Ganondorf (GTower). (A warp to the stage Link is in reuses his process's address: the
    track's (name, address) key then spans both.) Spawn an enemy: a Bokoblin, Moblin, Darknut (shield
    and cape), Chuchu, Keese, ReDead or Kargaroc 150 units ahead of Link, facing him, on the next game
    frame (wwhd::debug::RequestSpawn; WWHD_DEBUG_SPAWN's creation): the scratch route's DOWN three times
    and A, A spawns a Bokoblin there. The Bosses page also has Ganon's Tower's four refight rooms (Xboss0-3:
    Gohma with Valoo's tail, Kalle Demos, Jalhalla, Molgera; each boss present after the warp).
    **Boss rush** (the owner's idea; top page, "Boss rush (all, in order)"): the Bosses page's first eight in
    order with refights on (not Ganon's Tower's refight rooms: there a beaten boss sends Link on by itself,
    Gohma's death in an X stage calling dLib_setNextStageBySclsNum instead of setting the bit, d_a_btd.cpp). When the boss being fought is beaten (the game sets its dungeon's "boss beaten" bit:
    dSv_memBit_c::onDungeonItem(3), f_025B9098, onStageBossEnemy in the decomp) the next stage change the
    game asks for (the warp out) is turned to the next boss: its destination rewritten, its wipe kept
    (sixty.cpp's DebugStage); after the last, the game's own. A game over's restart isn't turned (the boss
    wasn't beaten): the same boss again. Another warp from the menu ends it. Checked with a test aid,
    WWHD_DEBUG_RUSHBEATEN=tick (the boss beaten at that frame), and a stage change after it: Gohma's room
    (the menu), then the "beaten" boss's exit to Outset went to Kalle Demos's room, Kalle Demos there. Not yet
    seen: a boss actually beaten setting the bit (the decomp: Gohma's death sequence calls onStageBossEnemy
    and creates the warp flower at its count 0x118; poking its sequence, m6E16 +0x6F32, to 100 stops at its
    event order). The log says "wwhd debug: a boss beaten" when the bit is set.
  - **Counts a call without a rule, in session bottom's types (shared enemy code too; regress.sh rerun)**:
    session qa's `tools/sixty/counter_audit.py` lists every field loaded, +-1 and stored back with no
    rule. For my processes (and the enemies item) each site was classified from its code (a scratch
    classifier, then read): keep:REG on the add where the count is tested right after it changes (a
    record form, a compare, or a reload and test), late on the store where it is tested before (`if (n)
    n--`). 57 rules: the enemies' shared code (5, from session top's list: counter_audit files them under
    VigaH 297), pots (5, session qa's list), Bokoblin, Darknut, Moblin and the Bokoblin's stick (8, in
    tick_rules.txt), Stalfos, Poes and the Moblin lanterns (earth.txt), Bubbles, Puppet Ganon and
    Ganondorf (ganon.txt, 14), rats, Helmaroc King, the ropes and barrels (fortress.txt), the Tower's
    statues (tower.txt), stakes (link_items.txt), Armos, Molgera and 252 (wind.txt), stones. Left on
    purpose: a ReDead's escape count (+0x4F4: stick-driven), stores setting a count from another value
    or a constant (02047BE8). The "not yet understood" ones, since read: Gohdan's 020E682C and the shared
    timers loop 02041A1C (late, 41c74c0); Ganondorf's +0x60B (021558EC), a countdown that keeps his two
    colliders' bit 0 off while it runs, tested before the - 1: late (poked to 20 on the gtower route at
    f8300: 20 ticks at both rates, 10 before); process 252's gameInfo +0x5B60 - 1 every 32 ticks of its
    whole-ruled count ((n & 0x1F) == 0, so its half step passed the test too; counter_audit filed it
    under the Stalfos by its code's place): whole (needs gameInfo +0x34 set: not run); once an event,
    not timers: Molgera's health (+0x3A1, a hit: her hit check waits for +0x1B5A, 6 after a hit, whole),
    the Stalfos' +0x3A9/+0x3A8 (stealItemLeft/BitNo: the grappling hook's steal; the hit check waits for
    m02F6, +0x412, 5 after a hit), the Darknut's +0x3F4 (armour pieces left, once as each flies off).
    Hits: a whole tick's collision pass (cCcS::ChkAtTg clears and sets the hit flags) leaves its hits for
    the half step and the next whole step; each of these waits for a post-hit window, so one hit counts
    once. **In a draw, only whole and keep work**: draws run every frame with a step of 1, where late
    and the step rules (split, *h, k@...) change nothing, and a converted process's draw stores stand.
    The Helmaroc King's tail_draw counted its tail's hiding (+0x40A, 020660E0) down every frame: late
    there (the counts round) was wrong, whole now (session top's catch). An audit of every non-whole/keep
    rule in code a draw method reaches (a call graph from the asm dumps) found one more, session top's
    205 (told). The Chuchu (CC 206; the trial in the fight route): two phases a call, its body's
    squash turned by m31C (+0x424, + 1000) and a hit's shake by sin(m348) (+0x450, + 0x3000), at 60
    twice as fast: spliti, now equal to 30's at each tick's end, halfway between.
  - **A ReDead's scream and grab at 60 (shared: sixty.cpp's StepInEvents, tick_rules.txt; regress.sh
    rerun)**: the same fight route against a ReDead (`WWHD_DEBUG_SPAWN=950:224,0,-201599,168,312125,0,
    f82d`): it screams, Link is startled (0xB8), frozen (0xCE) and held (0xCF, 150 ticks), all events
    with those actions, so all at 30. Added, and the two-actor event camera of the scream ruled
    (f_0253B168, TP's twoActor0EvCamera: k@ on its CtrCus and EyeCus approaches, ten sites); the grab's
    camera is the trans camera (ruled). At 60 the same timeline (a tick early), the ReDead's half steps
    through the grab, its joints within a few units of 30's, the scream's camera within 0.2 degree, the
    grab's swing (160 degrees in ~20 ticks) the same per tick, 1-2 ticks early.
  - **Enemies: a Bokoblin in the sea, the Stalfos' countdowns (round 2 enemies; session bottom)**: a fight
    on the Outset dock (scratch route: the save route, a Bokoblin spawned 120 units ahead facing Link,
    `WWHD_DEBUG_SPAWN=950:189,0,-201599,168,312125,0,f82d`; ZL held from f985, B every 15 frames from
    f1000): hit, knocked back ~35 ticks through the air (peak 256 against 263) into the sea, its stick
    knocked away on its own arc; then water_fail (f_020A3B18, its only per-call move) lowered it 1 a
    call: at 60 it sank twice as fast. `*h@f30` on that `pos.y -= 1` (tick_rules.txt): 1.0 a tick at
    both rates, deleted 120 ticks after landing at both. The stick's landing turns its angle x by a
    speed or the rest of the way to 0x3A00 for ~7 ticks (an inline chase: not ruled). The Stalfos (my
    earth.txt): six countdowns session qa's audit found (+0x20CD, +0x25AC, +0x202C, +0x21A8, +0x21C8, the
    head's 500-tick life +0x414), each tested right after: `keep`. M_Dai,15 with
    `WWHD_DEBUG_POKE=990:190,3e0,2,1`: 30 against 60 as before (mean 7 units apart, paths 326/335).
  - **Songs at 60: the playback, its end and the Wind's Requiem's change of wind (shared: sixty.cpp's
    StepInEvents, tick_rules.txt; regress.sh rerun)**: after conducting (0x9A, already stepping) Link
    plays the song back (0x9B, procTactPlay f_0243AC64), ends it (0x9C) and, for the Wind's Requiem,
    sends the wind (0xC4): about 400 ticks per song held at 30. All three added. Ruled: procTactPlay's
    countdown to the melody (mTactPlayTimer +0x6920, 10 calls; its zero test reads the register, so
    `keep`, not late, which would start the melody twice), the song camera's count (tactEvCamera
    f_0253754C: eye and centre at set offsets from Link) late, and the wind's camera (f_0253775C; no
    decomp; data names BirdFlyDist, UpCount, FollowCushion, FovyCushion...): k@ on its follows (0.02),
    the eye's height (0.05) and fovy (FovyCushion), its count late, and state 12's progress, a sum of
    the count each call, *h@. Scratch route (save route): UP 5 at f1000, RUP 24 at 1022, RLEFT 25 at
    1046, RRIGHT 24 at 1071 (3/4: no meter input), LLEFT 10 at 1300 and A at 1320 (the direction), A
    every 40 frames after: the same timeline at both rates (0xC4 f1363-1423), the playback and the
    overhead view equal, the wind's camera within 0.1 degree and 0.1 of fovy, half way at mid-tick.
    The view after a song (found by session top; fixed with its event-end fix, 8dd19ee): the song's
    last camera (f_02534964, TargetType 9) returns to the view the song camera saved on its first call
    (m11C 0) in gameInfo +0x5B0C-+0x5B28 (centre, eye, fovy, bank; a watch on it: written once at 30,
    at f1004, with the field view). At 60 it was written again at f1005 with the song camera's own view
    (yaw 178 against -11), and after the song the camera faced Link. The song is ordered during Link's
    whole tick at f1004, after the camera's whole step began stepping: the order's edge held its half
    step, so its late stores were lost, m11C among them, and the first call came again. Two fixes: a
    held half step's late stores are made there (below, "Late stores at an event's edge"), and the save's
    8 stores are whole (a first call on a half step, its count late, would save again after moving the
    eye). Now: written once (f1004w), m11C in step, the end view yaw -10.5 at both by f1468.
  - **Cutscenes at 60 (shared: sixty.cpp, overrides.txt; names in symbols.csv)**: JStudio cutscenes (dDemo)
    held everything at 30: Link's cutscene action (0xA9, daPy_lk_c::dProcTool f_0241FD7C: his place, angle
    and animation frame from his demo actor) wasn't in StepInEvents' list, and the cutscene's values change
    once a tick (dDemo_manager_c::update f_025291C8 calls stb::TControl::forward(1), f_0283D514; each
    TVariableValue is a curve or rate of age (whole frames, +0x4) x seconds a frame). Now 0xA9 steps, and
    while it does the whole tick's update is followed by forward(0) (no frame passes: the sequence's waits
    and commands stay on whole ticks) evaluating the values at age - 1/2 (update_time_ f_02839DB4 and
    update_functionValue_ f_02839DF4 overridden: (2 age - 1) x spf / 2), and the half tick starts with them
    at age again. The cast reads half way at its whole step and the tick's own values at its half step, as
    stepped processes do; the camera shows what it read a step late, so its whole frames equal 30's ticks.
    Test: Ganondorf's arrival in GTower (route gtower with WWHD_DEBUG_STAGE=920:GTower,0,0,-1 and
    WWHD_DEBUG_BOSS=1; a JStudio cutscene f971-8200): Link's actions identical, his animation frame 13.5 and
    14 against 30's 14 (his own stepped animations' halves alike), the camera's four moves (f2598, f2811,
    f4145, f7928) exact at whole frames and half way at half frames (before: the half frames repeated the
    whole). The title screen's opening at boot (a JStudio cutscene; at 60 from boot as play-60.sh runs, Link
    in wait 4, so the cast steps): its camera pan exact at whole frames and half way at half frames (it moved
    on the half frames only, a 30 Hz pan, before). events_test.sh's routes (none has a JStudio cutscene)
    identical; checks and regress identical.
    WWHD_60FPS_DEMOS=0 turns it off. How it was found: the demo manager's globals (m_control 101D5FE8,
    current file 101D6004, frame 101D6008) by d_demo.cpp's asserts; watching 101D6004 tells whether a
    cutscene is JStudio (the boss refights' intros are event cameras, not JStudio). Open: other JStudio
    cutscenes (the 100% save replays few); an unconverted cast member reads the half way values at its
    whole step and keeps them (half a tick behind at whole ticks).
  - **Houses' doors open at 60 (`tick_rules/doors.txt`, sixty.cpp)**: KNOB00 (305) converted and Link's
    door-open action 0xC1 (dProcDoorOpen) steps in events. The door's animation and Link's go through
    stepped helpers; one hand-made step, the exit's pull to the door's front (adjustmentProc f_021A3E90:
    pos x 0.8 + front x 0.2 a tick for 10 ticks, then put there), as an approach (`d:`/`k:` on its two
    factors) with its count `late`. Scratch route knob (WWHD_DEBUG_STAGE=920:Ojhous2,0,0,-1, Link's
    house: in through its door, LUP 14 back to it, A: out to Outset and out of the house there): every
    event and action on 30's tick; Link's entry and both exits on 30's path at whole frames with half
    frames between (judder.py 968-1014: whole and half moves 2.39 and 2.41 units, against a 30 Hz step
    before), the pull exact at half frames and put on 30's tick, the doors' frames in half steps (whole
    frames half a tick behind, Link's own animation half a tick ahead of the door's: not visible).
    The exit's camera (FIXEDFRM f_025314BC, then UNITRANS f_02531D20 with RelUseMask "or") ends on
    the door's other side at 60 on this route: its 'r' mode mirrors the eye when the camera's counter
    m080 (dCamera +0x80, +1 a tick, `late` in Run) is odd at the cut's start, and m080 starts from
    cM_rndFX(0x7FFF) at the camera's creation (each stage load), so the side is a coin flip at 30 too
    (30's run: 0x359D, odd; 60's: 0xFFFFFBC2, even; it counts once a tick at 60). Not a 60 fps matter.
    New probe detail: WWHD_STATE_CENSUS_TRACE's lines now start with the storing instruction ("at PC:").
  - **The Ballad of Gales warped nowhere at 60 (since songs stepped at 60, ab879b1), fixed
    (`link_items.txt`)**: the song's end (procTactPlayEnd f_0243B200, 0x9C, stepping in events) asks for the
    sea map on its first call (gameInfo +0x5BDB) and notes it (m3574, Link +0x69C4); its next call reads the
    destination chosen there and starts the warp, or ends the song without one. At 30 the map stops the
    game in between; at 60 the tick's half step came first, found no destination and ended the song (route
    warp: 0x9C one tick, then Link steered away at f1850; the Wind's Requiem, which the songs work was
    tested on, has no map). Both stores `late` now: route warp's song end, flight and arrival at the Tower
    of the Gods on 30's ticks (0x9C f1664-1928, the new stage at f2240 against f2241, Link within 15-43
    units). After it the boat lands 28 degrees off (the spin's ends a fraction of a tick apart, above), so
    the route's steering misses the ring of light at f3224 (a scripted route's matter, not the game's).
    The other songs' ends checked for the same pattern: the Song of Passing's restart is guarded by a flag
    (a scratch route like wind's with right, left, down: its timeline as at 30, the day changed and the
    stage restarted at f1383 against f1384); the Command Melody's change of player repeats harmlessly (the
    same partner; not testable: no companion on the 100% save); the Wind's Requiem's end sets a button.
  - **Late stores at an event's edge (shared: generate.py, ppc_ops.h, sixty.cpp)**: an event that
    begins, is ordered or ends during a whole tick holds the half step of the event's own processes
    that took a stepping whole step (sixty.cpp's edge, above). A `late` store waits for the half step,
    so those were lost: each such process's once-a-tick counts fell a tick behind 30's from the event's
    start. Now the generated else branch of a late store notes it on a stepping whole step (late_wr32
    and on, while g_rtLateNotes is set: its address, size and value), by process and in order;
    at the half step the notes are made if the edge holds it (as its step's stores, so the half tick's
    rollback keeps them) and dropped if it steps. The 10 late calls (event orders, which repeat) aren't
    noted. At 30 nothing changes (RT_LATE_TICK is always true). Run log line: "N late stores made for
    stopped ones" (the song route: 4 holds, 15 stores). events_test.sh (back, door2, talk, cuts, items,
    warp, door): base and new lines identical; regress identical.
  - **The Ballad of Gales' flight at 60 (shared: sixty.cpp's StepInEvents, tick_rules.txt; regress.sh
    rerun)**: the whole warp (the cyclone lifting the boat, the flight, setting it down) is an event with
    Link sitting in the boat (0x89 SHIP_PADDLE; steering is 0x88), not in the event-stepping list, so
    it ran at 30 (route `warp`, f1929-2425). Both added; then what steps there: the boat's lift
    (procTactWarp f_02482438) and arrival (procStartModeWarp f_02481FAC): the spin added to the heading
    (split@), speed.y + 1 and y += speed.y (*h@), the cyclone lowered 12 a call (*h@); their chases are
    cLib_chaseS/F's, already time-stepped by sixty_step.cpp's overrides (rules on those calls halved
    the spin-up twice: the flight took 160 ticks longer). The flight's camera (tornadoWarpEvCamera
    f_025383D0; no decomp): 100 calls bringing the eye to a view of the boat by (1 / countdown) x 0.15
    (k@ on 0.15, as on its fovy and bank: exact at the end, ~4% fast early), the centre after the boat by
    0.25 (k@), then 200 calls easing eye and fovy by 0.05 and the bank by 0.02 (k@); its countdowns and
    state changes late. And the camera's NotRun path (f_024FF6C0, while gameInfo +0x52E4 is set: the
    arrival, the ship setting the camera itself) counted m07C, m118, m11C, m108 and shook every call:
    Run's rules there too (late counts, the shake once a tick). Route `warp`: the flight lasts the same
    (ends f2424 against f2425), the boat's spin and climb tick for tick with 30's, the camera from f2055
    within a degree and 1-2% of distance (in its first 100 calls up to 7 degrees ahead: it starts from
    the ride camera's view, 17 units off, and closes about half the gap). After 40 turns of spin the
    boat lands facing 28 degrees differently (a fraction of a tick in when the spin starts and stops),
    so the route's timed steering misses the ring of light; with the heading poked to 30's on landing
    (`WWHD_DEBUG_POKE=2440:165,32a,2,a1df;2440:165,322,2,a1df`) the descent into Hyrule steps at 60
    too: in the courtyard within a few units and 0.3 degrees, in Hyrule the boat sinks at 30's 14-15 a
    tick, 2-4 ticks ahead (done f3401 against f3405). The boat's other event procedure, procZevDemo,
    moves by c_lib's approaches and speedF (all time-stepped).
  - **The follow camera's turn ramp a count ahead at 60, fixed (shared: tick_rules.txt; regress.sh
    rerun)**: followCamera's m38C (a hang's or crawl's turn ramp: m3B8 = m38C / 30, then m38C + 1, and
    its countdown and resets) and m392 (a spin's charge ticks: a ratio of m392, then m392 + 1) were
    `whole`: the change landed on the whole tick's step, so the half step, the tick's end, read the next
    count (the crawl route's trial: m3B8 0.4 against 0.367, "twice"). `late` now, as the other cameras'
    counts: both frames of a tick read the tick's count. Route `crawl` (Link over the Outset dock's end
    and hanging): the camera's turn round to the ledge, 12 degrees a tick, equal to 30's within 0.75
    degrees at every tick and its count with 30's (before: a count behind at each tick's end, 6-7
    degrees). Two things there aren't the camera: Link reaches the edge 2 ticks sooner at 60 (his crawl's
    speed follows its animation, whose phase is ahead, and the edge is found on a half step), and while
    he hangs at the edge the forward check (forwardCheckAngle: lines ahead along the camera's yaw, the
    ground's height where they land) sits on the dock's edge, so a degree of yaw flips it (the pitch
    bumps to 20 degrees at 60 for ten ticks).
  - **Item gets at 60 (shared: sixty.cpp's StepInEvents, tick_rules.txt; regress.sh rerun)**: opening a
    chest (Link's action 0xAD, DEMO_OPEN_TREASURE) and holding up what he got (0xAE, DEMO_GET_ITEM) held
    their events to whole ticks, as talks did (B18). Both added, and three cameras ruled (no decomp,
    all stubs): getItemEvCamera (f_025351A0: waits Timer1 = 27 calls, then approaches a clear view over
    Timer2 = 5 by t = count / Timer2: its count + 1 and the wait's end late, k: on t), transEvCamera
    (f_02531D20, TP's names: start-to-end by t = (m11C + 1) / Timer or a B-spline, cushioned: lag@ on
    t's count, k@ on its eleven Cushion sites) and the B-spline stepper both event cameras use
    (f_025C0C80, d2DBSplinePath::Step: its count late, lag@ on count x speed). Tests (scratch routes on
    the save route; `WWHD_DEBUG_SPAWN` now takes anglez, a chest's item): a heart piece at Link's feet
    (`950:255,00ffff07,-201622,168,312243`) and a chest 120 units ahead facing him
    (`950:292,000fff80,-201599,168,312125,0,f82d,400`, tbox 31; walk up, A): Link's actions at the same
    ticks at both rates (0xAE f975-1108; 0xAD f1031, 0xAE f1062, done f1259), the cameras equal to 30's
    at every tick's end through each transition (pitch within 0.1 degree) and half way between at mid-
    tick; the chest (TBOX 292), the item (255) and the get camera's trials clean. During the opening the
    camera holds the view it had at the event's start: 16 units further at 60 from the follow camera's
    own difference (see its lag lead).
  - **No longer seen (2026-10-05, ww-4 f504cfa): the door2 route's half-step push of a standing Link.** Link
    now stands still at both rates from f1137 to the route's end (the 13 units between them are from the
    route's opening turn and walk, 25 apart by f1037). The report, for the record: **on qa's `door2` route
    (no second door spawned) Link, standing, is pushed 38 units at 60 from f1379 (session top's report)**: after all of the route's input, only on half ticks (his
    position holds at whole ticks), by his BG correction (WWHD_STATE_CENSUS_TRACE on pos.x: f_024EF6B8 ->
    f_024F3574 -> f_024F2CA0), no actor within 800 units. It appeared with B19: the door's slam shake
    draws cM_rndFX each call and ran twice a tick before, so the random stream after the door changed.
    The half-step wall push while standing is the bug to find (a trial on Link at f1379).
  - **Bug B20, the warp light's rise ran at 30, now at 60 (shared: sixty.cpp's StepInEvents, a new
    `lag` step rule in generate.py, tick_rules.txt; regress.sh rerun)**: B18's cause again: rising in a
    warp light is Link's action 0xD2 (daPyProc_DEMO_WARP_SHORT, dProcWarpShort f_0242583C), not in the
    event-stepping list. Added, with his rise's acceleration `*h@` (link_items.txt) and the rolling event
    camera that circles him (f_025369BC; the decomp has a stub; data names as TP's rollingEvCamera):
    a call moves its centre toward the target by CtrCus (`k@f1` on the scaling calls) and puts the eye
    on a globe whose longitude and radius are the start's plus m11C x a speed. m11C counts whole ticks
    (`late`), so the whole tick's step would show the last tick's angle and the circle would turn every
    other frame: `lag@fN` (new; `lagi`'s float twin, for a count made a float) makes the count N - 1/2
    on the whole tick's step. Test: Cave01 room 0's light (`WWHD_DEBUG_STAGE=920:Cave01,0,0,-1` on the
    save route, walk off it and back: LUP 25 at f1030, LDOWN 30 at f1070): the warp lasts 119 ticks at
    both rates (it starts a tick sooner at 60, Link's walk), the camera turns 0.75 degrees a frame (30's
    1.5 a tick; its mid-tick yaw half way), its distance within 1.5 units and pitch within 0.3 degrees,
    Link's height within 0.2; the camera's trial over the rise clean, Link's speed equal.
  - **Bug B18, talks ran at 30, now at 60 (shared: sixty.cpp's StepInEvents; regress.sh rerun)**: Link's
    action while talking is 0xAA, which wasn't in the list of actions converted processes step under
    in an event (4 wait, 6 move, 0x9A conducting), so a talk held them to whole ticks. 0xAA added, and
    the talk camera (talktoCamera f_02508A60, 'TALK'; the decomp has a stub) ruled: at each cut it sets
    up N and a weight W and approaches by (N - count) / W, W losing N - count and its own count + 1 a
    call (the shield camera's scheme): k: on the factor, the two late. qa's `talk` route (Windfall's
    dock, the sailor): the talk steps at 60, its camera's fovy equal to 30's through the cut, yaw and
    pitch within a degree, distance 370 against 382; the sailor turns to within 0.9 degrees. The cut's
    set-up runs on both half steps of its first tick (m11C late); left, as the result is this close.
    Open, older than this (also with `WWHD_60FPS_EVENTS=0`): after the talk the next A starts a new
    talk 30 ticks sooner at 60 (f1270's A at 60, f1300's at 30): a cooldown after a talk counts a step.
  - **Bug B19, the camera's shake too strong at 60, fixed (shared; regress.sh rerun)**: shakeCamera
    (f_024FC108, from Run) steps its pattern a bit a call and flips the offset's sign a call: at 60 a
    bomb's 10-call shake (one held in Link's hands on the Outset dock) ran in 5 ticks, the sign flipping
    every frame, a harsher and shorter jitter. It runs once a tick now (whole on Run's call), its
    offsets held on the half tick as 30 shows them on both frames.
  - **The hookshot's pull camera (hookshotCamera f_02511B5C, 'HOOK'), ruled (shared)**: its centre
    approaches Link's point by a factor vector it stores (k@ on the stores) and its fovy the style's
    (k@). Its radius, yaw and latitude approach the globe from the last frame's eye to that centre: a
    self-referential update (the eye follows at 1 - k of the centre's speed) that is the same a tick at
    60 unconverted; with them converted the eye's pull-back grew only 2/3 as fast, so they're left. A
    lesson for the other cameras: an approach toward a target built from the camera's own last eye
    isn't a fixed-target approach.
  - **The bait (ESA 221), converted**: the pieces the bait bag throws (all-purpose bait itself on a
    button: item 0x82, `WWHD_DEBUG_POKE=950:0,1046F0BA,1,b;950:0,10474C6C,1,82`; Y throws it,
    procFoodThrow): its timers, its flight (pos += speed, then speed.y -= 3: late) and its bob on water
    (WWHD's: the sine of a phase + 3000 a call). Thirteen pieces with random speeds: at 60 their flights
    go as far and as high as at 30's (181-327 against 166-310 units, rises 34-76 against 31-75).
  - **Beamos (Bemos 233) fired early at 60, fixed** (session top's report: a Beamos spawned facing Link
    on the Outset dock, `WWHD_DEBUG_SPAWN=950:233,00000002,-201622,168,311893`; the beam hit ~4 ticks
    early, its scorches came and went early): its eye's charge counts (m6AE to m6B0, then m6AC to 5),
    the red eye's search delay and the broken eye's count went a call each, and its eye searches
    (blue_eye_search f_02326E4C, red_eye_search f_02327968) grew and ended its beam (m588, m5A8) a call
    each. keep on the counts, h of the beam's + 1s (tower.txt). After: the charge counts equal 30's on
    every tick, the beam grows at 30's rate half a tick ahead (its aim crosses the threshold on a half
    step) and ends ~1 tick early (was 4): the `< 5` check flips on the half step after the limit.
  - **The Z-target camera ran its approaches twice a tick at 60, fixed (shared; regress.sh rerun)**:
    lockonCamera (f_025052E8, work area tagged 'LOCK') had no rules. A ZL lock-on on a ChuChu spawned on
    the Outset dock: four ticks in, the eye's distance 281 against 241, the letterbox (dCamera +0x5FC,
    GC's mTrimSize, f_024FF8A0: 0 to 90 by 0.25 a call) 84.9 against 68.6, the fovy and the yaw ahead.
    36 rules (tick_rules.txt): k@ on every approach's factor (its cushions, the centre offset globe, the
    view's yaw, latitude, radius, fovy, the charge's latitude), d@ on the blocked view's R x 0.75, the
    charge's count late and the blocked-view countdown whole. After: the letterbox equal, the distance
    within 1% (241/245, 274/278, ... 392/392), fovy and pitch within 0.6 degrees, the yaw ~0.9 degrees
    ahead (the route's half-tick input). The follow camera's distance after a lock ends is ~2% shorter.
  - **First-person views turned twice as fast at 60, fixed (shared; regress.sh rerun)**:
    dCamera_c::CalcSubjectAngle (f_02506964), which Link's setBodyAngleToCamera (f_02416E90) calls on
    each of his steps, adds the right stick's y times a rate to the view's pitch fraction m388 and sets
    the yaw's m384 from its x, which Link then turns by. At 60 the bow's, hookshot's, grappling hook's
    and telescope's views pitched and turned twice as fast: 12 frames of right stick up gave 67.1
    degrees against 33.55, and a grappling hook thrown at a stake flew over it. *h@ on the stick's
    factor at its four sites (tick_rules.txt, beside subjectCamera). After: the pitch 33.48 against
    33.54, Link's heading after 10 frames of right stick left within 10 units. Found with a stake
    (KUI 250, `ffff0400` like the stages' own) spawned over the sea off the Outset dock on Link's
    first-person sight line; the camera process's view is +0x264 eye, +0x258 centre.
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
    Swinging on the grappling hook's rope: a stake (KUI 250, `ffff0400` like the stages' own) spawned
    over the sea off the Outset dock on Link's first-person sight line
    (`WWHD_DEBUG_SPAWN=950:250,ffff0400,-201481,751,311511`; inputs 1000 Y 3, 1020 RUP 12, 1070 Y 3,
    1130 LUP 70; the scratch script bin/rswing.sh in session bottom's worker dir): the hook wraps, Link
    is pulled under it (proc 0x77) and swings (0x78) with the stick's pump, then the swing dies down. The
    swing's phases add 1 + a little a tick in four copies the compiler made (by the quadrants' cases):
    with all four and the pump ruled the phase stays within 0.04 rad of 30's over 200 ticks (it ran 1.4x
    to 2x before). The boomerang's lock-ons: three ChuChus (CC 206) spawned on the dock ahead, Y held, the
    view swept across them with the right stick, Y released: the same lock, throw and return at 60 (the
    lock markers' pulse is held to whole ticks). The hookshot's pull: on Windfall (sea room 11; spawn 8
    with `WWHD_DEBUG_STAGE=920:sea,8,11,-1`, the point is the spawn's id; the hookshot poked onto Y after
    the warp, `1035:0,1046f0ba,1,13;1035:0,10474c6c,1,2f`, as the warp resets the buttons), Y, 2 frames
    of right stick left, 3 up, Y: it sticks in a wooden post (an HS-flagged target, ~(1710, 1880,
    -202829)) and pulls Link at 30's ~61 a tick, landing within ~20 units (a tick early: the input's
    half tick); the pull's camera is hookshotCamera (ruled, below). Not driven yet: climbing vines,
    pushing and pulling blocks, hiding in a barrel. Everywhere Link's camera-relative heading is ~1.5
    degrees off 30's (the camera's control angle, below), so long routes drift a little (a wall's corner
    reached later, then apart).
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
    swims another way. The view turns with it: its eye-to-centre yaw (camera process +0x264 eye, +0x258
    centre; an earlier reading of +0x268/+0x25C took the wrong fields) is behind 30's by the same amount
    as the control angle, e.g. 1.68 degrees and 306 units when the swim circle ends. followCamera's trial
    sites there are all ruled (k@ and k75@ on its approaches, the turn ramp m38C/30 counted on whole
    ticks): the gap is likely the k75 approximation with a factor that changes every tick. In a
    sustained turn it isn't a rate: Link swimming in circles off the Outset dock (stick up-left, ~190
    ticks) has the control angle turning +10496 against +10570 over 48 ticks, ~300 (1.6 degrees) behind
    30's throughout; kindan's 16 ticks were the turn's start. Not changed. A lead (session bottom,
    from the hookshot camera): followCamera's yaw starts each call from the globe of its own last eye
    to the moved centre (decomp l.371, `local_484`), which is rate-invariant unconverted, then
    approaches the target yaw by m3B8 cos V (k75@); the two together, and the eye's own 0.75
    smoothing after, may not add up to one tick's in a sustained turn.
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
- **Session qa** (from 2026-10-04: the owner's bugs, B1... on the progress page; `publish.sh bug list`):
  - **Doors (B1, B3, B4), fixed (shared: `sixty.cpp`; regress.sh rerun)**: the owner's Link turning left
    into the wall at a Dragon Roost Cavern door, turning round and walking off a ledge at a Tower of the
    Gods door, and doors that wouldn't open again. One cause. DOOR10 (300) is converted, and while Link's
    action is wait or move a converted process steps in an event too. The door's demo action ends its
    event with `dComIfGp_event_reset()` (bit 8 of dEvt_control_c's event flags, gameInfo +0x52B8) and goes
    back to its wait action; the event's control clears the partners' event commands on the next whole
    tick (its check, in the event manager's runProc). At 30 nothing runs between. At 60 the door's half
    step ran its wait action with its command (actor +0xF8) still 3, took that for a new door event and
    went back into its demo action (+0x4EC = 3) for good: it could not be opened again, and, executing
    whenever Link is in one of its rooms, it answered every later door event of that room (its own
    slide, smoke and `setGoal`, the goal Link's event walk heads for, computed along its own direction):
    Link walked off toward the stuck door's side. Now no converted process steps on the half tick after
    an event's end is asked (`EventEnding`, as after one is ordered), and
    `dEvent_manager_c::getIsAddvance` (f_025447C8, a new override) reads 0 on a converted half step: the
    manager advances cuts on whole ticks, so a half step saw the cut as new again and ran its init twice
    (the door's smoke: a second emitter, its shake never counted; `setGoal` again from where Link had
    walked to). `WWHD_60FPS_EVENTEND=0` turns both off. Tests: routes `back` (through the door behind
    Link and back again from the other side) and `door2` (then a second door spawned ahead) with
    `WWHD_DEBUG_STAGE=920:M_NewD2,5,14,-1` (Dragon Roost Cavern's rat room, by its lower door) and
    `WWHD_STATE_TRACK=168,300`: before, at 60 Link stood at the door on the way back and the door's
    +0x4EC ended 3; now he goes back through as at 30 (5 units apart), the door ends 1, its smoke fields
    (+0x420, +0x421) as at 30, and the first door's state no longer changes during the second door's
    event. The event still ends ~2 ticks sooner at 60 (cuts end on the half tick they finish in). Session
    top takes the half tick a converted process loses at each event's start and end (it skips its half
    step there): only processes not part of the event may finish their tick.
    A door's rooms are in its x angle (front room `& 0x3F`, back room `>> 6 & 0x3F`; the stage file's
    TGDR chunk: M_NewD2's rat room 14 has two doors to the outside, room 3). `WWHD_DEBUG_PLACE` stops at
    the first wall on the way (the ground check's line from the old position): place Link within a room.
  - **Link's reset flags (B15: the Wind Waker's notes), fixed (shared: `sixty.cpp`)**: the note display
    (dMetronome_c::melodyShow) lights a diamond when `checkTactInput()` is set: bit 0x01000000 of Link's
    reset flags (daPy_py_c::mResetFlg0, WWHD's Link +0x3C0), "what happened in his last step", cleared in
    the middle of each of his executes. With Link stepping at 60 a process that runs on whole ticks saw
    one step only: those executing before him his half step's flags (the display never saw the note,
    judged on whole ticks), those after him his whole step's (a roll into a tree, a hammer blow, an arrow
    shot landing on a half step would be missed the same way: checkFrontRollCrash, checkHammerQuake...).
    fpcM_Execute's override now shows a whole-tick process both steps since it last looked (the last
    whole step's flags and the last half step's together) and gives Link and every process stepping at 60
    his last step's alone (his own tests before the clear must not see a flag twice). The word in his
    memory, and so in a track of him, is the combined one between executes.
    `WWHD_60FPS_RESETFLAGS=0` turns it off. Test: the warp route's song, captures at f1420-1505 at 30 and
    60 (`CAPTURE_30=1 tools/sixty/tests/capture.sh`): before, the diamonds at 60 showed none, none, then
    one note in the first place; now 1, 2, 3 notes as at 30.
  - **The hookshot's reticle (B21), fixed (`link_items.txt`; shared: `generate.py`)**: aimed at an actor,
    the yellow lock came on and went off every other frame, its sound with it. The hookshot's wait state
    (f_021789B4) clears its "hookable" flag, puts the sight's cross point back at the far point and enters
    the sight's collider every step; the collision pass (whole ticks) sets the hit point and the flag
    again through the collider's callback. The converted hookshot's half step did the resets without a
    collision pass after it, so Link's whole step saw nothing hookable and his half step the tick's hit.
    The resets are `whole` now (02178A48, 02178D74/7C/84), the lock's frame count (26 a cycle, a sound
    at 0) `keep` in setHookshotSight (f_02432D80), and the frame-0 sound `whole` there and in the rope's
    aim. For 02178D74, a `stfsu`, `whole` and `late` now take update-form stores (`generate.py`: the
    skipped store still updates its register). Test: route `hook` (the hookshot on Y by
    `WWHD_DEBUG_POKE`, a Bokoblin spawned behind Link, turn round and aim) with `WWHD_STATE_TRACK=168,169`:
    Link's lock flag (+0x58ED) changed 64 times in the aim at 60, 2 at 30; now 2, and its frame (+0x58EE)
    counts one a tick.
  - **The hurricane spin (B9), fixed (`config/US_v0/tick_rules/link_qa.txt`, qa's own Link rules)**: the
    owner's "over too quickly, the dizziness too short". procCutRoll (f_02441DC0) counts its time left
    (Link +0x6916, 90 ticks) and turns the body 14000 a tick (+0x6934); procCutRollEnd (f_02442134) counts
    the dizziness; procCutTurnMove (f_02442470) the 47-tick charge. Each ran a step: at 60 the spin lasted
    45 ticks and turned 28000 a tick. Rules: `late` on the two counts tested at entry, `spliti` on the
    turn, `keep` on the charge (its 0 is tested on the register at once). Route `spin` (turn round on the
    Outset dock, B to draw, B held 110 frames, let go; `WWHD_DEBUG_POKE=1133:168,6916,2,1e` shortens the
    spin to 30 ticks so the dizziness fits on the dock): spin 53 ticks and dizziness 61 at 30, 53 and 60 at
    60 (before: the count lost 2 a tick). Link's procedure numbers are the decomp's daPyProc enum
    (0x56 CUT_ROLL, 0x57 CUT_ROLL_END, 0x59 CUT_TURN_MOVE, 0x2C HANG_FALL_START, 0xAA talk).
  - **Stairs (B12), fixed (`link_qa.txt`)**: setStepsOffset (f_023FF47C) looks a tick's move ahead
    (`current.pos + speedF` along Link's angle) for a step up, lifts Link onto it and lowers the model by
    0.7 of the step (Link +0x6A1C, eased back). A half step moves half as far: lifted early, Link was
    still short of the step on the next frame, the ground check put him down again, and the step was taken
    twice (the model dropped another 17.5: his eye height, +0x394, went 250, 237.6, 246.4 where 30 has 250,
    258.8, 263.8: the owner's judder). The look ahead and the slope's allowance are a step's move now
    (`*h` on the two loads of speedF). Dragon Roost Cavern's entrance stairs (route `walk` after
    `WWHD_DEBUG_STAGE=920:M_NewD2,0,0,-1`): the eye rises every frame at 60 and passes 30's values (each
    step is taken half a tick to a tick later: Link is nearer the step when lifted).
  - **Bushes and small trees being cut (B6, B7), ruled (`config/US_v0/tick_rules/plants.txt`)**: only the
    plants' sway was ruled; a cut bush's rise, fall, drift, pitch and fade (dWood::Anm_c::mode_cut,
    f_025CD474), its push animations' counts and phases, a cut tree top's slide and fall and a shaken
    tree's spring (dTree_data_c::animation, f_025C6D40) ran a step: twice as fast at 60. The plants'
    trial (`WWHD_60FPS_TRIAL_PLANTS=1`, route `slash`, a bush `WWHD_DEBUG_SPAWN=950:266,0,-201622,138,312160`
    or a tree `950:435,00000017,...` 80 units ahead of Link on the Outset dock) is clean after the rules but
    for the tree top's last slide step (it lands on a half step: 1 unit short). Not looked at on screen.
  - **Tools (session qa)**: `tools/sixty/tests/whole_only.py DIR PROC FROM TO` lists a tracked process's
    words that change on whole ticks only in a 60 run (what moves at 30 inside a converted actor);
    `capture.sh` with `CAPTURE_30=1`. A bug that shows as "X steps twice" is found fastest with the trial
    over a route that does X (`WWHD_60FPS_TRIAL=168 WWHD_60FPS_TRIAL_TICKS=a-b`), then `grep` its output for
    the field's values.
  - **Puppet Ganon's "black screen" (B5): the game's own scene, closed as not a bug**. On the 100% test
    save Puppet Ganon was never beaten: event bit 3F10 (set by its death, read by its create: beaten, a
    rope and a stake instead of the boss) is clear, 3B02 (Ganondorf's speech before the fight seen; while
    clear the room loads layer 8) is set, 4002 (the tower top's first visit) clear. Walking into GanonK
    starts the fight with no aid; `WWHD_DEBUG_BOSS` doesn't reach it (it answers the dungeons' "boss
    beaten" bit, isDungeonItem item 3, only while set, and writes nothing). Its death sets 3F10 and loads
    `GanonK` point 4 layer 9 (d_a_bgn.cpp): a cut to black, Link close up, a dark shot of Link raising
    the Master Sword with Ganondorf's lines ("Yes, surely you are the Hero of Time, reborn..."),
    Ganondorf with Zelda by the bed, a fade, Link free in the room. The same at 30, at 60 and on the
    unmodified game (below). For a boss rush: a refight has to restore the flags its boss reads (here an
    event bit, not the dungeon's bit) and expect the game's after-fight sequence to run for real.
    **Every `WWHD_DEBUG_*` aid is off unless its variable is set**: normal play (`tools/play/play.sh`
    sets none) never runs them.
    The save's flags: the save data is on the heap (the pointer at 0x101F84DC; 0x145B7B60 on route
    `save`), dSv_event_c at +0x644 (isEventBit f_025B8B94, onEventBit f_025B8B68: byte `flag >> 8`, mask
    `flag & 0xFF`); `WWHD_60FPS_WATCH=addr,...` prints heap words each frame (watch.txt beside the binary).
  - **The unmodified game with the same pokes: `tools/reference/refpoke.py`**. The reference Cemu has
    none of the test aids (they are overrides of recompiled code), so an experiment made with them
    couldn't be repeated on the original. refpoke writes the emulator's guest memory from outside
    (/proc/PID/mem; it finds the guest's base by the game's code and times itself by the game's frame
    counter and stage name): `refpoke.py PIDFILE "stage=sea+120:stage GanonK,0,0,-1" "stage=GanonK+180:p
    243 14eb4 2 6;p 243 14eb6 2 0" "+900:p 244 602 2 6;p 244 604 2 0" "+400:p 245 11e66 2 3;p 245 11e68 2
    0;p 245 11e90 2 2710"` beside `tools/reference/run.sh` (a copy of /wwhd/opt/cemu-src/bin in a folder
    of its own as `CEMU_BIN`, `CEMU_SHOT_FRAMES=2400-5400/100`) is B5's run: warp, Puppet Ganon's three
    forms, its death. Processes are found by name in the heap; they sat at the same addresses as in our
    runs.
  - **The glide's flicker (B11): the Mirror Shield's glint, fixed (`link_qa.txt`)**. Link, the leaf and
    the camera move evenly in a glide at 60 (`tools/sixty/tests/judder.py`), but a bright halo flashed on
    the shield on his back on some frames (the brightness around the shield over twelve consecutive
    captures: 559 551 563 568 546...; flat with Link unconverted). setItemModel (f_02403C24, from Link's
    execute) runs the shield's glint by hand: a texture animation that starts at random (`cM_rnd() < 0.02`
    a call) and then advances a frame a call (WWHD: two live copies, frames at Link +0xE0C, 30 long, and
    +0xF1C, 60 long; a third at +0x4814). On both frames of a tick it started twice as often and played
    twice as fast. Rules: the frame's + 1 `*h@`, the start's 2% `*h@` on its fcmpu (the two tests of a tick
    are independent: other processes draw from the stream in between); the hookshot's frame (+0x6A44) and
    two more hand-run frames (f_0240B7F4: +0x5618, +0x5708) the same. 2995 ticks idle on the dock: 44 and
    28 glints of 28.7 and 59.0 ticks at 30, 32 and 27 of 29.1 and 59.5 at 60. Not found yet: the decomp's
    other simpleAnmPlay callers (the sword's glow, the magic armour, the leaf's gust, the water ring), if
    WWHD keeps them: look for `fadds` of the 1.0 register with an `fsel` or a frame-count compare after.
  - **The Moblin's block (B14), fixed (`tick_rules.txt`, mo2's section)**. "Its spear blocks attacks that
    should get through": when Link's sword lands, damage_check sets m05B6 (+0x85A) to 25, and while it
    counts down the Moblin can't go to its defence (fight_run: `m05B6 == 0`, a cut type it has been hit
    with before, and being Link's ZL target); so a combo's next cuts land. The execute (f_021C9B1C) counts
    eight such fields down by hand after its timers' loop, and none had a rule: the trial that found the
    Moblin's rules ran on one walking the dock, where they are all zero. At 60 the 25 ticks were 13.5.
    Rules now on those eight (`whole`), on the other counters of its execute (the parry opening's ticks,
    the blink, the alarm's and the camera's counts) and of Mo2_move (f_021D0A14, its actions inlined: the
    ticks Link stands close before the shove, whose 87.5-unit guard also bounces the sword, the attack's
    wait, the stun's count). Tests: the fields poked to 25 on an idle Moblin
    (`WWHD_DEBUG_POKE=1000:188,85a,2,19`) last 24 ticks at both rates (12 before); route `mob` (hold ZL,
    slash every 14 frames) with `WWHD_DEBUG_STAGE=920:Cave09,0,14,-1` (the Savage Labyrinth's two Moblins,
    a flat round room: a good arena, the Outset dock is not, a hit knocks either side into the sea): before
    the rules a Moblin went to its defence 13.5 ticks after a hit, now 33.5 (26 at 30). Moblin fields
    (WWHD): the action +0x8C2 (4 fight_run, 5 fight, 7 shove, 10 defence), its mode +0x8C4, the timers
    +0x848. **The same trap for every enemy tested idle**: countdowns that only run in a fight. The
    fights are chaotic (the random stream), so compare counts and delays, not paths.
  - **The same for Bokoblins and Darknuts (session qa; top agreed: qa takes the land enemies, top the sea's)**.
    d_a_bk.cpp and d_a_tn.cpp are the Moblin's file twice over: after the timers' loop the execute counts
    six to eight fields down by hand, the "can't block" one (bk m0310 +0x428, tn m03F2 +0x50A) 25 on a hit,
    the "can't be hit" one (bk +0x426, tn +0x508) 5; tn's m1402 (+0x1552) is the armour's shake. All `whole`
    now, with the counts before a jump attack or a stun (`keep`). Poked to 25: 24 ticks at both rates
    (12 before). Arenas (Savage Labyrinth, `WWHD_DEBUG_STAGE=920:Cave09,0,ROOM,-1`, Link lands in the
    middle): room 13 Bokoblins, 14 Moblins, 16 a Darknut; Cave10 room 10 Stalfos. Route `mob` fights
    whatever ZL targets; scratch summary per enemy: hits (the no-block field rising), the delay to its next
    defence, Link's cuts and bounces (proc 0x41/0x46, 0x5A). Fields: bk action +0x4A2, mode +0x4A0; tn
    action +0x596, mode +0x594 (10 its defence). **How to find them in any enemy**: list every stored
    +-1 in its functions (a field loaded, +-1, stored back) and tick off those with a rule; the trial only
    sees what runs in its test. **Not done: the Stalfos** (ST 190, session bottom's rules in earth.txt): its
    head's 500-tick life m02F8 (+0x414, f_024963A4 at 02496644, then == 0: `keep`), and the execute's byte
    countdowns at +0x20CD, +0x25AC, +0x202C, +0x21A8, +0x21C8 (HD's layout past the GameCube's fields:
    read them first). Not looked at: ChuChus, Keese, ReDeads, Poes, Wizzrobes, Floormasters, Bubbles,
    Peahats, Armos, Miniblins, Magtails, Kargarocs, Mothulas. The ground smoke's count of bk, mo2 and tn
    (a puff a call while it runs) is left alone: as many puffs as at 30 in half the time.
  - **Link's own countdowns (session qa, the same audit on his code)**: ~35 of his timers still counted on
    both frames, each an action or a guard half as long at 60. The one that changes play: after a hit he
    can't be hit again for 30 ticks (mDamageWaitTimer, +0x3B0, changeDamageProc): 15 at 60. Also the
    window a turned stick counts for a spin attack (m3524), the ticks on a slope before he slides
    (m3526), the wait before the next arrow (m355E), the Deku Leaf's gust's ticks (m353A), mQuakeTimer,
    and the actions' own counts (mProcVar0.m34D0 +0x6916...): the waits after the parry cuts, the spin
    attack, the jump attack's landing, a weapon's swing, a missed grab, a ladder's rung, the rope and
    bottle actions, and in procTactWait the three ticks after a beat before the stick is judged (1.5 at 60:
    a late stick read as the wrong note, perhaps part of B15). Rules in `link_qa.txt`, the kind by how the
    count is tested (`late` if before the - 1, `keep` if after, else `whole`). Poked to 25 on the dock
    (`WWHD_DEBUG_POKE=1000:168,3b0,2,19`): 24 ticks at both rates (12 before); route `cuts` (a spin
    attack, two jump attacks): procCutTurn 17 and procJumpCutLand 14 ticks at both (16.5 and 13 before).
    His action table: 12-byte entries at 0x10036DF4 (.rodata), the function in the third word, in the
    decomp's daPyProc order. One more, found later (the execute keeps Link + 0x448 in r26, so the audit
    printed its field as +0x652A of r26): m3522 (+0x6972), the window to continue a sword combo, set by
    each cut and counted in the execute; at 0 the combo's step is cleared. 12 ticks at 60 against 24:
    combos dropped where 30 continued them (B27). `late` on its store (0240D640). Looked at and left: his
    face (playTextureAnime f_023FBCEC, from the execute at 0240D78C: the texture frames m3530/m3532 go
    + 1 a call in some branches and a blink starts at random): standing on the dock he blinks as often and
    as long at both rates (27 and 25 blinks of 47 ticks in 2995), the frames there come from a frame
    control. If a face is seen running fast in some action, `whole` on that call is the fix.
  - **`tools/sixty/counter_audit.py`** (new): per converted process, every stored counter (a field loaded,
    +-1, stored back) with no rule on its store or add. Run it after converting a type and after a fix;
    with no argument it lists every converted process that still has such a countdown (79 lines on
    2026-10-05: NPC_PEOPLE 9, pots 5, the boat 5, the Stalfos 9, the bosses 2-7 each...). Each is a
    candidate, not a bug: read it in the decomp (input-driven counts and state numbers want no rule).
  - **The audit's other fixes (session qa)**: a barred door's 65-tick wait after its room is cleared
    (door10/kddoor m2A1 +0x3BD, `late`; B24); the Magtail's five hand-counted timers and its +0x5AC count
    (poked in DRC room 8, `920:M_NewD2,0,8,-1`: 24 ticks at both rates, 12 before); the Kargaroc's unk_326
    and unk_340 ("can't be hit", 5 or 50) and the Keese's m314 (10 or 50), poked on ones spawned on the
    Outset dock: 24 at both, 12 before (B25). By the code's idiom only, not seen counting in a test (zero
    or held in the idle state the pokes reach): the Boko Baba's +0x480, the Peahat's +0x460, +0x46C and
    +0x48E, the Bubble's +0x406: `whole` or `keep` leaves a count per tick either way. The Stalfos is
    session bottom's (done there), the sea's creatures top's. Not looked at: Poes, Wizzrobes, ChuChus (no
    countdown left), Armos, Miniblins, Mothulas, the bosses, NPCs, pots (5), the boat (5): run the audit
    with -v and read each.
  - **Timer loops without a rule** (counter_audit.py reads indexed stores since da8b79e; an earlier scratch
    grep had missed them): the Keese's four timers (+0x4C0, 0219AE7C) and the Peahat's five (+0x480)
    had none: every wait of theirs ran at twice the speed (poked to 25 on ones spawned on the dock: 12
    ticks at 60 against 24). The Keese's: `whole sthx` (24 at both rates now, its path over 75 ticks
    unchanged); the Peahat's: session top's `keep` on the add (023CC2D8, in its sealife commit, tested in
    action: a Seahat's attack cycle ran every 60 ticks against 120).
    The same audit lists, for their owners: Mothula's seven timers (204, 0214D95C), the Octorok's six
    (227, 023C14D8), the flame lift's (91, 021C31A8), per-element counts of Gohma (234, 020EA8CC), Kalle
    Demos (235, 020B04AC), Gohdan (240, 020E682C), the shared enemy code (02041A1C) and the fences
    (SAKU 398: 02464F60, 024650F4, 024651D4).
  - **Before every deploy: `tools/sixty/tests/predeploy.sh`** (session qa, after B28): 23 scripted routes at
    30 and 60 with the current build, FAIL when Link ends more than 150 units from the 30-tick run's end
    (another outcome), WARN from 40; ~8 minutes on the desktop (the 30 runs are kept and reused). B28 (the
    Ballad of Gales never warped at 60, for a day of deploys) got through because every test compared a
    build with the one before, and "unchanged" was still broken; this one compares with 30. Run it on the
    tip you deploy; a FAIL is a bug to log before the owner finds it.
  - **B16's hunt, automated (nothing found)**: Windfall's spawn points (sea room 11's PLYR ids: 0 and 16
    the dock, 20 the town centre without a door, 1-13 and 15 the buildings' doors; `920:sea,20,11,-1`
    stands Link among the Killer Bees). 1200 consecutive captures (every second swap for 40 s, the camera
    turning in steps) scanned by a scratch script: a frame scores min(change from the frame k before,
    change to the frame k after) - change between those two, for k = 1, 4, 12 captures; a glitch that
    appears and vanishes scores high, steady motion near 0. Top score 0.036 (people walking): no streaks
    there. The dock-to-quay walk (route `wfw`) drops Link into the water at 60 now: use spawn 20.
  - **For the owner's recordings**: `~/wwhd-play/play-60-rec.sh` (desktop only, made by qa) is play-60.sh
    with the flight recorder on (Link, the camera, arrows, ships); F9 writes the last 20 s to
    `~/wwhd-play/flight/`. B8, B10 and B16 wait for one.
  - **Open (session qa)**: B16 (Windfall's streaks: the colours of the man by the gate's clothes; not
    reproduced from the dock, on the quay by the gate over 20 s, or after a talk; route `wfw` strafes Link
    from the dock to the quay at 30, at 60 he slips off its edge: his path is 5 units off); B8 (the bow's
    pull at 30: Link's and the arrow's fields all change on both kinds of frame while aiming; needs the
    owner's view and moment); B10 (Beedle's ship measured the same at both rates: needs the owner's where
    and what). After a talk ends the next A starts a new talk ~30 ticks sooner at 60 (session bottom's
    note; no countdown of Link's or the townsperson's steps twice in the trial there: the message
    window's own closing, probably).
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
