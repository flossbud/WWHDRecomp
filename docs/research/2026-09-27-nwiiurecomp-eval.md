# nWiiURecomp evaluation — 2026-09-27

Phase 0 of [`../getting-started.md`](../getting-started.md). Target: our **US v0** `cking.rpx`.
Upstream: `BlackLineInteractive/nWiiURecomp` @ `d911856` (2026-08-05). This is the only
public branch (`main`), with 0 forks and no CI.

Everything below was run on this server (Debian 13, GCC 14.2, 4 cores, no GPU).

## What we ran

| Step | Result |
|---|---|
| Build host libs + CLIs (`cmake` + `ninja`) | Builds in about 40 s CPU. GCC 14 needs `-Wno-error=array-bounds` (false positive at `-O3`). |
| Unit tests | 35/36 as shipped. `native_test` (the golden lifter test) fails on `fdivs`. |
| `nwiiu-analyze --any-title` | 1.7 s. 50,116 functions, 302,966 relocations, 1,540 unresolved control-flow records (exit 3, documented as normal). |
| `nwiiu-recompile` (as shipped) | **Fails**: 1,611 blocks rejected, every one of them `fdivs`. |
| Same, after a one-line fix | **The whole US v0 RPX lifts**: 2,351,001 instructions → 3,746 C++ shards (695 MB) in 4.5 s. All 36 tests pass. |
| Compile generated program (`wwhd-native`) | **Builds**: 38 min wall-clock (134 min CPU) on 4 cores, giving a 310 MB binary. |
| Run `wwhd-native` headless | Stops at **exactly the same place** as the interpreter (6,226,708 instructions, same PC and fault), in 0.35 s against 0.91 s. The recompiled code agrees with the interpreter, so the stop is a runtime/HLE problem, not a lifter one. |
| `nwiiu-run` (interpreter + HLE, headless) | Stops at **6.2M instructions** with a null read (`lwz r9,0x10(r24)` at `0x0278659C`) right after `OSGetThreadSpecific`. This looks like a runtime HLE gap. Not investigated further. |

**The one-line fix:** the generator's instruction validator (`native_generator.cpp`, op 59
branch) omits `xo5 == 18` (`fdivs`). The emitter and the interpreter both implement it.
Adding `18` to the `(20 || 21) && frc == 0` condition fixes it. Worth reporting upstream.

## What it is (from AGENTS.md and the code)

* **Pipeline:** analyzer (RPX parse, relocation, CFG) → lifter (PPC → C++ text calling
  header-inline semantics) → shards + `program.cmake`. There are two hosts:
  * `wwhd-native`: SDL3 plus an in-tree GX2/Latte renderer.
  * `wwhd-recompiled`: a patched Cemu that `dlopen`s the module. Per AGENTS.md, only
    this one "actually plays the game".
* **Granularity is the basic block, not the function.** Each block is an
  `extern "C" recomp_<addr>(cpu, memory)` that sets `cpu.pc`, and a dispatcher looks up the
  next block. This is closer to an AOT-compiled JIT cache than to XenonRecomp or N64Recomp,
  which emit one C function per guest function with native control flow.
* **Differential testing** (`NWIIU_DIFF=1`) replays each block on Cemu's interpreter
  in-process and compares all state. It only exists in the Cemu port.
* **Runtime:** about 17.6k lines of its own code: interpreter, scheduler, coreinit/FS HLE,
  a GX2 runtime, and a 2k-line header-only renderer.

## What is *not* in the public repo

* **The Cemu port.** Commit "remove Wind Waker HD recomp port patch" deleted
  `patches/cemu/` and `extern/Cemu`, and `configs/` is gone too. The path that plays the
  game is not public.
* **The trunk.** AGENTS.md says the LLVM-IR backend, region formation and the shader AOT
  table live on `feature/wwhd-direct-llvm-backend` ("the trunk, 571 commits"). That branch
  is not published. `main` is a snapshot of `feature/shader-extractor`.
* **A clear licence.** The README says GPL-3.0, but `LICENSE` contains the text
  `404: Not Found`. Reusing its code would need either the author's confirmation or a
  GPL-3.0 project.

## Assessment

Useful as a **reference and a proof of feasibility**:

* Its analyzer and lifter cover all of WWHD, paired singles included.
* Its HLE and GX2 code show which OS surface WWHD actually touches.
* The in-process differential tester is the right verification design.

As the **base of this project** it is a weak fit:

1. **Wrong granularity for our end state.** We want a recomp where recompiled functions
   are replaced one at a time with hand-written C (60 fps interpolation, fixes). That
   needs function-level output with symbol names, which is what XenonRecomp/N64Recomp
   produce. Block-level dispatch also caps performance; upstream's answer is the
   unpublished LLVM trunk.
2. **The part that actually runs the game isn't public**, and what is public faults early
   in startup on our build.
3. **Bus factor and licence:** one author, no CI, a malformed LICENSE, and the WWHD port
   was pulled from the public tree in August.

## Recommendation

Write **our own recompiler at function granularity**, XenonRecomp-style, fed by the Ghidra
function list and jump-table data from Phases 1–2. Borrow *ideas* from nWiiURecomp freely:
the PSQ/GQR semantics, the HLE surface map, and the differential-test design. Borrow *code*
only once the licence is confirmed and we have decided whether this project is GPL-3.0.
Build the runtime on Cemu's OS and Latte components (MPL-2.0).

Keep `nwiiu-analyze` as a **cross-check** for our own function and CFG recovery. It is fast,
and its 50,116-function count is a useful baseline.
