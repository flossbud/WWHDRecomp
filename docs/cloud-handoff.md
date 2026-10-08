# Cloud sessions: handoff (2026-10-07; tailnet mode and performance pointers 2026-10-08)

This is for Claude Code sessions that run in the cloud (claude.ai/code) on the GitHub mirror
`flossbud/WWHDRecomp`. Local sessions work on the owner's machines, which have the game; cloud
sessions don't. Read this first, then `CLAUDE.md`, `README.md`, `docs/handoff.md` ("What the project
is", "Hard rules", "Known facts and gotchas") and `docs/recompiler-design.md` (D1, D2, D6, D9, D11,
D12, D18, D19, D20, D21).

## Tailnet mode (from 2026-10-08): the cloud session is a worker session

This section overrides the rest of the file where they differ: the owner gave the cloud session access to the worker,
the worker that has the game.

- **Joining the tailnet.** At the start of every session, run `bash tools/cloud/tailnet-setup.sh`.
  - The cloud environment's variables carry the keys and the logins; the script's header lists them.
  - It joins the owner's tailnet in userspace and writes `~/.ssh/config` for `worker` and the PC.
  - It sets up this checkout as session `cloud`: worker directory `/wwhd/WWHDRecomp-cloud`, `.session` = cloud, and the
    `worker` remote.
  - Set `WWHD_ON=worker` if the environment hasn't.
  - From then on, `tools/worker/sync.sh`, `tools/worker/w` and `tools/worker/job` work exactly as for a local session.
    Read `docs/handoff.md` ("Parallel sessions", "Infrastructure", "Hard rules").
- **What it can reach.** Only the worker's SSH, plus the owner's PC's SSH for deploys. On the PC, the key runs nothing but a
  receiver: `status` (is the game running), and writes of the deploy files into `~/wwhd-play`.
  - Don't try to reach anything else on the tailnet: the rules block it, and it isn't yours to probe.
  - The login names are the owner's own account names. They live in the environment variables and in
    `tools/worker/desktop.env` (gitignored), never in git.
- **Game data.** The game, its traces, dumps, captures and the recompiled output stay on the worker. That's the same rule as
  for local sessions: don't copy them into this sandbox, into a PR, or into a message. Small numbers and verdicts are fine
  (a position error, MATCHES, an md5).
  - `/wwhd/data/ghidra-out` and decompiler output are the game's code in another form: read them on the worker
    (`tools/worker/w`), and quote only what a commit needs as evidence.
- **Your goal (the owner's): the game at full speed at 60 on the worker.** Its 6-core CPU is power-capped to 45-60 W, a
  stand-in for the Steam Deck's CPU: if the game holds 60 there, the Deck's CPU side will.
  - Its Intel Intel iGPU is weaker than the Deck's GPU, so treat the render thread's numbers there as pessimistic.
  - Work through `docs/research/deck-plan.md` in order, items that need the game included.
  - Measure on the worker with alternating A/B rounds of the same binary, flag on and off
    (`tools/sixty/perf/perf-ab.sh` is the desktop version: your first step is a the worker variant that runs in its worker container on the Intel GPU, headless; see its README and `perf-baseline.md` for how qa measured). Report relative gains: the worker is
    noisy, so use more rounds rather than fewer.
  - The main session confirms absolute numbers on the owner's PC later.
- **Landing a change.** You merge into `ww-4` yourself, with the full gates a local session uses: checks all MATCH
  (traces, streams, sound, diff, captures PSNR inf), regress reviewed line by line, predeploy 0 FAIL, plus
  `predeploy.sh gohmatail gohmarock`.
  - Speed work must leave 30 bit-identical and the 60 results unchanged, unless a change is meant to move them and says
    so.
  - Push to `worker` only: `git push worker HEAD:ww-4`. the worker mirrors every branch pushed to it on to GitHub by
    itself, within seconds, through a post-receive hook with a repo-only deploy key and a guard that refuses anything
    with the owner's name. Its log is `/wwhd/logs/github-mirror.log` on the worker. Don't push to `origin` yourself:
    the sandbox can't, and it doesn't need to.
  - A refused push means someone landed first: rebase and re-gate (code) or just rebase (docs).
  - Local sessions may be working too. Before a long gate, claim the item: `tools/progress/publish.sh claim ID "what"`
    from this checkout. Tell them in `docs/handoff.md` when you land something that touches shared code.
- **Deploying to the owner's PC.** Only with the owner's OK, asked each time in this chat; say what's in the build.
  - Then run `WWHD_DEPLOY_KEY=~/.ssh/wwhd_cloud_deploy tools/play/deploy-cloud.sh "$WWHD_DESKTOP_SSH"` after a full
    build on the worker.
  - It refuses while the game is running. If it does, wait and ask again.
  - Mark the bugs it fixes: `publish.sh bug fixed ID "fixed: deployed ww-4 SHA"`.
- **The progress site** (http://WORKER_ADDR:8765 on the worker; `tools/progress/README.md`). Keep your part of it
  current with `tools/progress/publish.sh` from this checkout: it writes as session `cloud` (`.session`) over SSH to
  the worker.
  - `publish.sh now "TEXT"` when you start something and as it changes (one line, cheap; e.g. "deck-plan item 3:
    descriptor sets, gating").
  - `publish.sh claim ID "note"` before a work item: its ids are plan.json's queue, e.g. `next-deck`, or
    `publish.sh item ID NAME` for one that isn't listed. Then `publish.sh step ID DONE TOTAL` for its bar, and
    `publish.sh done ID "result"` when it lands.
  - Bugs: `publish.sh bug start|ready|fixed ID "note"` as their state changes ("ready" when the fix is in ww-4,
    "fixed" once deployed); `publish.sh bug note ID "finding"` for findings; `publish.sh bug add TITLE DETAILS` for a new
    one you find.
  - The queue itself (plan.json `queue`, `queue_note`, `known_issues`): edit and commit it like code (docs-only, no
    gates), push, then run `publish.sh` (no arguments) to republish the page.
  - Don't run `publish.sh usage`: those meters are the local sessions' Claude usage, not yours.
  - `publish.sh shot` takes a capture from the worker to the page (it stays on the worker).
  - `publish.sh retire` when you stop for good.
  - A web session (WW-10) also sweeps the page: if your line looks overwritten, set it again.
- **Identity.** Commit as flossbud (the setup sets it), and never write the owner's real name or account names anywhere.
- **Reporting.** The owner reads on a phone.
  - At each landed item, say in a few lines what changed, the measured gain, the gates, and what's next.
  - Add your entries to `docs/handoff.md` (a "Session cloud" paragraph, like the local sessions' wrap-ups).
  - You can't see the credit balance. When the owner says to stop, finish or park the step in hand: commit WIP on a
    `ww-4-cloud` branch on the worker, with notes.

## Where things stand

WWHDRecomp is a static recompilation of *The Legend of Zelda: The Wind Waker HD* (Wii U, USA v0
`cking.rpx`):
- **Code:** the game's PowerPC code becomes C++ through a Python generator, `tools/recomp/`.
- **Runtime:** that C++ links against a runtime in `src/`, which includes Cemu's OS libraries, many
  of them forked into our tree.
- **Graphics:** a "null GPU" (`src/gpu/null_gpu.cpp`) consumes the game's GX2 command buffers and
  hands draws to our Vulkan renderer (`src/gpu/vk/`), which is derived from Cemu's.
- **Verification:** a patched, deterministic Cemu is the reference. Every change is checked
  bit-exactly against it at 30 fps: OS-call traces, GPU command streams and sound.

The main achievement is **true 60 fps**: the game's logic runs at 60 ticks a second and plays as it
does at 30. This isn't interpolation.
- **How it works:** each converted process steps twice a tick, governed by per-address tick rules
  in `config/US_v0/tick_rules*`, plus overrides in `src/overrides/sixty*.cpp`.
- **Coverage:** all ~450 actor types are converted, and each is checked tick by tick against 30.
- **Next:** uncapped (a variable time step) builds on it.

## Why cloud sessions now

- **The Steam Deck is too slow (bug B75).** It runs the game at 35-55% speed, 22-34 frames/s. The
  game thread is CPU-bound:
  - on an desktop CPU it needs ~40% of a core for real time;
  - 82% of that is the recompiled game code itself, spread flat with no hotspot;
  - the GPU-command thread uses another ~25%.
- **A rival project exists:** https://github.com/ZeldaWWHDRecomp/ZeldaWWHDRecomp (MPL-2.0).
  - It reaches Windows, macOS, Linux and Android.
  - It draws natively (Vulkan/Metal) instead of consuming GX2 command buffers.
  - Its 60 fps is interpolation, and its "true 60" is experimental.
  - The owner wants ours to match and beat its reach and speed while keeping our true-60 lead.
  - A study of it is being written locally. When it lands it will be `docs/research/rival-study.md`;
    read it if it's there.

## What a cloud session can and can't do

- **No game here.** There's no ROM, no `.rpx`, no extracted assets, no Ghidra project and no
  generated recompiler output (`build/recomp` is generated from the game and never committed).
  You can't run the game, the routes, the checks or the predeploy.
- **You can:**
  - read and change our code and the generator;
  - build the runtime **without** the generated program (`src/CMakeLists.txt` falls back to the
    interpreter when `build/recomp/func_table.cpp` is absent);
  - build and run the instruction fuzzer (`tools/recomp/fuzz/`: emitted code against Cemu's
    interpreter, one instruction at a time);
  - write unit tests;
  - set up CI;
  - port to other platforms and compilers.
- **Local verification after you:** the local main session merges your branch into `ww-4` only
  after running the real checks on the owner's machines:
  - `tools/sixty/tests/checks.sh`: traces, streams and sound must MATCH;
  - `regress.sh`;
  - `predeploy.sh`, 0 FAIL.

  So keep changes reviewable and say in your PR what needs checking with the game.

## Rules (hard)

- **Never commit game data.** No `.rpx/.rpl/.wua/.wud`, no extracted assets, no decompiler output, no
  generated recompiler output, no shader caches, captures, saves or core dumps. `orig/` and `build/`
  stay gitignored. If a tool would produce game-derived output, it doesn't go in git.
- **Branches:** work on `cloud/<topic>` branches and open a PR into `ww-4`.
  - Never push to `ww-4` or to any other session's branch (`ww-4-top`, `ww-4-bottom`, `ww-4-qa`, ...).
  - The local sessions push to a separate remote (the worker); the main session mirrors `ww-4` to
    GitHub.
- **Commits:** `git -c user.name="flossbud" -c user.email="224492734+flossbud@users.noreply.github.com" commit`, with a
  message ending `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Write messages like the
  existing ones: what changed and why, with numbers where there are any.
- **Determinism at 30 is sacred.** The 30 fps path must stay bit-identical to the Cemu reference.
  - Floating point stays strict: `-ffp-contract=off`, no FMA contraction, no fast-math. Fused
    multiply-add changes rounding, and the traces would stop matching.
  - Any speed change must keep the generated code's results identical. If a change can only be
    checked with the game, say so in the PR.
- **Licensing:** Cemu-derived and rival-derived files are MPL-2.0. Keep their license headers and
  note the origin, as the existing Cemu-derived files do (`src/README.md`).
- `CLAUDE.md`'s worker rules (`tools/worker/*`, the worker) are for local sessions. In the cloud, your
  sandbox is the build machine.
- The owner reads on a phone. Keep updates short and plain, and give choices as a numbered list with
  a recommendation.

## Setting up a build in the cloud

The local build uses three inputs. Recreate them in the sandbox, and write it as a script
(`tools/cloud/setup.sh`) so CI can use it too:
1. **Cemu** at the pinned commit `c717fcab` (https://github.com/cemu-project/Cemu, with
   submodules) plus `tools/reference/cemu-patches/0001-0016`, applied in order with `git am`.
   `tools/worker/setup-volume.sh`'s `cemu` step is the reference recipe. Its dependencies come from
   vcpkg through Cemu's manifest. The first build takes a while (11-12 minutes with 10 jobs on the
   worker).
2. **SDL 3.4.10**, static, with video (X11, Wayland), audio and Vulkan. See `setup-volume.sh`'s
   `sdl3` step.
3. **Our build:** `src/build.sh`, i.e. CMake + Ninja, Release, clang:
   `cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
   -DCEMU_SRC=... -DWWHD_SDL3=... -S . -B build/wwhd`. The worker's image is Debian trixie
   (`tools/worker/Dockerfile` lists the packages).

Without `build/recomp`, `wwhd-null` links with the interpreter. That's what CI can build and what
the platform ports must keep building.

## Tasks, in priority order

Take them one at a time, each on its own `cloud/<topic>` branch with a PR. Ask the owner before
starting anything not on this list.

### 1. A reproducible cloud build and CI (`cloud/ci`)
- **Work:**
  - `tools/cloud/setup.sh`: Cemu at `c717fcab` plus the patches, SDL3 and the vcpkg dependencies,
    cached where possible;
  - a GitHub Actions workflow that builds `wwhd-null` (no recomp) and the fuzzer on Linux x86-64
    and runs the fuzzer.
- **Done when:** the workflow is green on a PR, and the README says how to reproduce it.

### 2. Portability: Linux arm64 first, then Windows (`cloud/arm64`, `cloud/windows`)
- **Known x86-only spots:**
  - `src/runtime/fiber/FiberUnix.cpp`: an x86-64 context switch in asm; add arm64, D19's arm64
    item.
  - `src/os/coreinit/coreinit_Spinlock.cpp`: `_mm_pause`; use `yield` / `__builtin_arm_yield`
    on arm64.
  - `src/os/coreinit/coreinit_Thread.cpp`: `_mm_setcsr` sets flush-to-zero. On arm64 that's FPCR's
    FZ bit, but check what the reference's float behaviour needs: denormal handling must match
    the interpreter's.
  - `src/runtime/profile.cpp`: guarded already, so check the fallback.
  - Search `tools/recomp/runtime/ppc_ops.h` and the emitter for x86 assumptions (intrinsics,
    `long double`, endianness helpers).
- **Cemu's own code:** it has to build on arm64 too. Find out how far Cemu upstream supports
  Linux aarch64, and patch what's missing as new `cemu-patches`.
- **CI:** GitHub has `ubuntu-24.04-arm` runners, so add an arm64 job. The fuzzer on arm64 is a
  strong check that the emitted code's semantics hold on another ISA.
- **Windows:** later, with clang-cl or MSVC. Cemu builds on Windows upstream; ours adds
  `FiberUnix`, and `FiberWin` may be needed.
- **Done when:** CI builds and fuzzes on arm64.

### 3. The recompiled code's speed: a design, then a prototype (`cloud/codegen-speed`)
- **The cost today:** every guest register lives in the `PPCInterpreter_t` struct (D2). `ctx` is
  `__restrict`, and the compiler already keeps registers in host registers within straight-line
  code, but every call and exit writes them all back.
- **Levers, in the design doc's words:**
  - a liveness analysis: store only what a call or exit needs;
  - D19's context switch and queue fast path;
  - the cycle accounting (D6: one cycle an instruction, yields in place).
- **The cycle accounting:** it's what makes the checks deterministic, but real-time play doesn't
  need it exactly. Measure what it costs, and design a real-time build that drops or coarsens it
  while the checks build keeps it.
- **Other levers to try:** `-O3`, and `-march=x86-64-v3` for the Steam Deck's Zen 2 (but
  `-ffp-contract=off` must stay); LTO across shards.
- **What you can test here:** semantics with the fuzzer, and speed with synthetic PPC functions.
  The real measure (`tools/reference/timing.sh` on a route) is local.
- **Deliverable:** a design note (`docs/research/codegen-speed.md`) with options, expected gains
  and how each stays bit-exact, then a prototype behind a generator flag.
- **Read first (2026-10-08):**
  - `docs/research/perf-baseline.md`: per-thread CPU, the game thread's split, and what 60 adds to it.
  - `docs/research/deck-plan.md`: the Steam Deck plan.

  The game thread is what limits the Deck: an estimated ~19 ms a frame in a fight at 60 against a 16.7 ms
  budget. So a codegen gain there counts directly. The deck plan's "deeper cut of the half frame" is its largest
  lever after these items. The sound's 5-6x cost at 60 (the plan's item 1) is fixed (c9fc438).

### 4. Native graphics groundwork (after the rival study lands)
- The owner wants graphics drawn natively and efficiently. Wait for `docs/research/rival-study.md`.
  It will describe how the rival intercepts GX2 at the API level and translates shaders ahead of
  time.
- Then write a design (`docs/research/native-gfx.md`) for ours: how the GX2 calls map to
  Vulkan, the shader translation ahead of time, and how it's verified without the bit-exact
  command stream (captures within tolerance, as G3 does today).
- Prototype only what can be tested without the game.
- **Read first (2026-10-08):**
  - `docs/research/rival-study.md` (it has landed; §4.3 and §6 cover their texture tracking and performance
    work);
  - `docs/research/deck-plan.md` (seven items in priority order, each with its gain, effort, risk and gate);
  - `docs/research/texture-tracking.md` (page write-protection for textures, designed, not built);
  - `docs/research/perf-baseline.md` (the render thread's profile per draw).
- **Which deck-plan items need the game:** almost all of them, because their gates are the checks' captures and
  an A/B (`perf-ab.sh`) on a route.
  - **Local only:**
    - item 2's open work (below);
    - item 3: render-thread skips, each step byte-identical captures plus an A/B;
    - item 4: static actors in the half frame, gated by the hz30 sweep;
    - item 5: the half step's journal, regress and predeploy;
    - item 7: Link's half step;
    - the quick tests on the Deck.
  - **A cloud session can prototype:**
    - item 6's runtime half: `src/runtime/write_watch.{h,cpp}`, meaning the page stamps, protect/unprotect, the
      `SIGSEGV` handler chained before Cemu's crash handler, `HostWrite`, and a `sigaltstack` per guest host
      thread. All of it is unit-testable without the game: protect a buffer, write to it, check the stamp,
      check that a fault outside the range still reaches the old handler. The `texture.cpp` integration and
      the kernel-write audit's verify mode need the game.
    - item 3's code changes, built but unmeasured, if they're kept small and one per commit, so they can be
      A/B'd locally one at a time.
  - **Either:** the Android port's platform layer (the deck plan's Android section). It's weeks of work and
    comes before any of the plan's Android items.
- **The lazy GX2DrawDone (deck-plan item 2) landed off by default** (7123e87, ef9fad9; `WWHD_LAZY_DRAWDONE=1`).
  It saves 1.42 ms of the game thread's frame at 60 on the desktop. Open, all local:
  - a run on the owner's GPU with a window;
  - an A/B there with vsync on and off;
  - a run under the Vulkan validation layer (not installed on the desktop worker; a cloud session could check
    the code against the spec's present/semaphore rules: `PresentSemaphore` in `src/gpu/vk/present.cpp`,
    `SubmitFrame` in `renderer.cpp`);
  - then on by default.

  A latent hazard found on the way: a frame that fills the 256 MB upload ring mid-frame submits early and moves
  a few pixels. No route comes near that today.

## Reporting

- **Before you stop:** at the end of each task, or when you stop, push your branch, open or
  update its PR, and write in the PR description:
  - what changed;
  - what you verified here;
  - what must be verified locally with the game, with the exact commands;
  - any risk to determinism.
- **Tracking:** add a line for the task to `docs/handoff.md`'s "Cloud work" section (create it
  under "After that" if it's missing).
