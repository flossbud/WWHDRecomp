# Cloud sessions: handoff (2026-10-07)

This is for Claude Code sessions that run in the cloud (claude.ai/code) on the GitHub mirror
`flossbud/WWHDRecomp`. Local sessions work on the owner's machines, which have the game; cloud
sessions don't. Read this first, then `CLAUDE.md`, `README.md`, `docs/handoff.md` ("What the project
is", "Hard rules", "Known facts and gotchas") and `docs/recompiler-design.md` (D1, D2, D6, D9, D11,
D12, D18, D19, D20, D21).

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

### 4. Native graphics groundwork (after the rival study lands)
- The owner wants graphics drawn natively and efficiently. Wait for `docs/research/rival-study.md`.
  It will describe how the rival intercepts GX2 at the API level and translates shaders ahead of
  time.
- Then write a design (`docs/research/native-gfx.md`) for ours: how the GX2 calls map to
  Vulkan, the shader translation ahead of time, and how it's verified without the bit-exact
  command stream (captures within tolerance, as G3 does today).
- Prototype only what can be tested without the game.

## Reporting

- **Before you stop:** at the end of each task, or when you stop, push your branch, open or
  update its PR, and write in the PR description:
  - what changed;
  - what you verified here;
  - what must be verified locally with the game, with the exact commands;
  - any risk to determinism.
- **Tracking:** add a line for the task to `docs/handoff.md`'s "Cloud work" section (create it
  under "After that" if it's missing).
