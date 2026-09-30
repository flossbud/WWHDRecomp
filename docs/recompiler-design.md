# Recompiler design

Status: draft, 2026-09-28. Revised the same day: **native graphics from the start**. Cemu's GPU
emulator (Latte) is not part of the product; see D12–D16. Inputs: [`getting-started.md`](getting-started.md) (Phase 1 function boundaries),
[`research/2026-09-27-nwiiurecomp-eval.md`](research/2026-09-27-nwiiurecomp-eval.md), and source reading of
XenonRecomp, N64Recomp and Cemu (`main` @ `c717fcab`, 2026-09-24). Line and file references are to
those trees.

## Goal and non-goals

**Goal.** Translate every function of US v0 `cking.rpx` ahead of time into C++, and run it natively
as a standalone program. There are two parts:

* **Graphics:** the game's GX2 graphics API is implemented natively on Vulkan, and its shaders
  are translated ahead of time.
* **Everything else:** Cemu's Cafe OS libraries (threads, memory, filesystem, audio, input).

It must:

* be correct first, with every step checkable against Cemu: its interpreter for the CPU, and
  an unmodified Cemu build for rendering;
* allow any single function to be replaced with hand-written C++, which is how enhancements
  and decompiled code land later;
* never ship game code: the translation runs on the user's machine from their own dump.

**Scope: single screen, Pro Controller** (owner decision, 2026-09-28). WWHD has a proper
Pro-Controller mode, chosen on its Controller Selection screen. The recomp is designed around it:

* **Input:** one TV-style screen, driven through `padscore`/KPAD (`KPADReadEx`).
* **The GamePad** is reported as *not connected*. Its screen output is never presented (see D13
  and D17).
* **Second-screen support** (for dual-screen handhelds such as the AYN Thor) is a later feature
  of *our own* design, built on overrides and our renderer, not a reimplementation of Nintendo's
  GamePad model.

**Non-goals for v1.**

* Speed beyond "comfortably full speed".
* Other titles or versions.
* A GUI.
* Reimplementing Cafe OS. We reuse Cemu's HLE libraries; replacing them is a possible later
  stage, like N64Recomp's `ultramodern`.
* Emulating the Wii U GPU. There is no PM4 command processor and no Latte in the product.

## Architecture

```
 build time (user's machine)                        run time
 ─────────────────────────────                      ──────────────────────────────────────────
 cking.rpx ─┐                                       wwhd (native executable)
 functions.csv ─┤                                   ├─ generated/  ← recompiled guest functions
 jump_tables.csv ├─► recompiler ─► generated/*.cpp  ├─ runtime/    ← dispatch, HLE calls, helpers,
 symbols.csv ───┘   (tools/recomp)                  │               yield, diff mode, overrides
 content/ shaders ──► shader recompiler ─► SPIR-V   ├─ gx2/        ← native GX2 on Vulkan: state,
   (sharcfb, sarc, gsh)  (tools/shaders)            │               surfaces, draws, TV only
                                                    ├─ Cemu Cafe   ← RPL loader, coreinit scheduler,
                                                    │  (MPL-2.0)     HLE libs, IOSU/FS, audio, input
                                                    │               (no Latte, no gx2, no TCL)
                                                    └─ frontend    ← SDL3 WindowSystem, no wxWidgets

 reference (not shipped): upstream Cemu with our patches (tools/reference), run under Xvfb +
 lavapipe, which produces reference frames and OS-call traces for the same scripted input
```

**The seam.** We keep Cemu's boot path as it is:

1. `RPLLoader` loads and relocates the RPX (text at `0x02000000`, data from `0x10000000`) and maps
   imports to HLE stubs in the trampoline area at `0x00E00000`.
2. coreinit starts its scheduler.

Graphics are not in that path: GX2 is our own module (D12), and Latte is never started.

We replace guest *instruction execution* in two places:

* `__OSFiberThreadEntry`'s run loop (`Cafe/OS/libs/coreinit/coreinit_Thread.cpp:1346-1377`);
* `PPCCore_executeCallbackInternal` (`Cafe/HW/Espresso/PPCScheduler.cpp`).

Both call `native_dispatch(hCPU)`. It looks up `hCPU->instructionPointer` in the function table
and calls the native function. On a miss it falls back to Cemu's interpreter.

This is deliberately a different seam from nWiiURecomp's. That project hooked the JIT entry and
copied register state into its own struct and back on every block, which made its
block-granularity output necessary.

## Decisions

### D1. One C++ function per guest function

We follow XenonRecomp (`recompiler.cpp`), with the input taken from our own analysis:

* **The function list** comes from `config/US_v0/functions.csv`: 39,705 functions, audited to be
  contiguous, each starting at its entry.
* **Signature:** `void f_0200ECD4(PPCInterpreter_t* ctx)`. When `symbols.csv` has a name, it is
  attached as an alias.

How each branch instruction is translated:

| Guest | Emitted |
|---|---|
| `b`/`bc` within the function | `goto L_<addr>;` (a label is emitted only at branch targets) |
| `b` to another function's entry | tail call: `[[clang::musttail]] return f_X(ctx);` |
| `bl` | `ctx->spr.LR = next; f_X(ctx);`, then check the return (see D4) |
| `blr`, `bclr` (conditional) | `return;` / `if (cond) return;` |
| `bctr` listed in `jump_tables.csv` | `switch (index) { case i: goto L_<case_i>; }` |
| `bctr` otherwise | indirect tail call through the function table (D5) |
| `bctrl` | indirect call through the function table (D5) |

All 294 of WWHD's jump tables are runs of `b` instructions that the `bctr` jumps into, so the
switch cases on CTR's slot address (table + 4k) and the slot's own `b` goes on (fixed in M3; until
then the cases were the final targets and never matched).

**Unknown instructions are errors.** The generator fails loudly rather than emitting a stub. Any
exception must be added to a named allowlist.

**Output layout:**

* 256 functions per `.cpp` shard.
* A shard is rewritten only when its content hash changes, to spare rebuilds.
* A generated `func_table.cpp` holds `{guest address, host function}` pairs.

**Why this model:**

* Function granularity is what lets a hand-written C++ function replace a guest function cleanly.
* The host compiler optimises whole functions, not isolated blocks.
* The known failure modes (jump tables, non-contiguous functions, branches into another
  function's middle) are exactly what the Phase 1 audit rules out. A `b` whose target is neither
  inside the function nor a function entry is a generator error.

### D2. Register state is Cemu's `PPCInterpreter_t` itself

Generated code reads and writes `PPCInterpreter_t` fields directly
(`Cafe/HW/Espresso/PPCState.h`). This means:

* Cemu's HLE handlers run on the same state with no marshalling.
* Fiber switches inside HLE calls just work.
* Diff mode (D8) can hand the same state to the interpreter.

How the Espresso registers map onto it:

* **GPRs:** `gpr[32]` (u32).
* **FPRs:** `fpr[i].fp0` / `fp1` are doubles. Paired singles use both halves; scalar ops use `fp0`.
  **Callee-saved FPRs hold full paired singles**: the GHS save helpers store both halves
  (Phase 1 finding).
* **CR:** `cr[32]`, one byte per bit. `cmp` writes four bytes, and branches test a byte.
  `mfcr` (251 uses) packs; there is no `mtcrf`.
* **XER:** `xer_ca` / `xer_so` / `xer_ov`.
* **LR / CTR:** `spr.LR`, `spr.CTR`.
* **GQRs:** `spr.UGQR[8]`, written only 8 times in the whole binary (UGQR2–5 set-up).
* **Reservation:** `reservedMemAddr` / `reservedMemValue`, the same semantics as Cemu's
  `stwcx.`: address match plus compare-and-swap. There are only 21 `lwarx`/`stwcx.` pairs.

**Later optimisation.** XenonRecomp keeps CTR, XER, CR and the reservation in host locals within
a function. We can do the same once everything works, but state must be written back before any
call, HLE call or yield point.

### D3. Memory is Cemu's flat 4 GiB region

Guest address `ea` maps to `memory_base + ea`: one reservation, identity-mapped, big-endian. Loads
and stores are macros with byte swapping (`LOAD_U32(ea)` and so on), in one header, so diff mode
and debugging can instrument them.

Semantics that must be exact:

* **`dcbz`** (82 uses) zeroes the whole 32-byte line.
* **`lmw`/`stmw`** (7,276 / 4,480) and **`lswi`/`stswi`** (463 each) move multiple words.
* **`psq_l`/`psq_st`** dequantise and quantise according to the GQR, using Cemu's tables.
* **`isync`** (5,518, GHS epilogues), `sync` and `eieio` are no-ops; `dcbf` and `dcbst` are no-ops.

### D4. Imports call Cemu's HLE handlers directly

When Cemu links the RPX, each import resolves to a trampoline word `(1<<26) | hleIndex`
(`rpl_mapHLEImport`, `Cafe/OS/RPL/rpl.cpp:746`). WWHD has 408 imports (397 functions it calls,
10 data symbols its code references; the count of 426 used until M3 included the 18 import-section
symbols).

* **Direct calls.** The generator knows every import symbol. A `bl` to an import emits
  `hle(ctx, IMPORT_coreinit_OSGetTime)`. At boot, a table maps each import to Cemu's handler via
  `PPCInterpreter_getHLECall`.
* **The return check.** Cemu handlers return by *setting* `instructionPointer`, normally to `LR`.
  After every HLE call the generated code checks `ctx->instructionPointer == LR`. If they differ,
  the handler redirected: MEM allocator forwarding (`coreinit_MEM.cpp:543-562`) tail-calls a guest
  function. That target is then dispatched as a tail call. WWHD imports no `OSLoadContext`, no
  coroutines and no setjmp/longjmp, so the harder redirect cases don't occur (checked 2026-09-28).
* **Indirect calls into the trampoline area** (`0x00E00000`) arrive through function pointers
  such as `MEMAllocFromDefaultHeap`, a *data* import. The function table (D5) maps trampoline
  addresses to the same HLE path.

**As built (M3, `src/runtime/imports.cpp`).** Imports are bound from guest memory, not by name:
the runtime follows every relocated branch to an import (3,643 sites) to its trampoline and reads
the HLE opcode there, and rebuilds each data import's address from its relocated immediates (58,
some with an addend: `_iob+0x10` is stdout). All sites of an import must agree, and the result is
cross-checked against Cemu's name tables (`osLib_getFunctionIndex`, `osLib_getPointer`): 0
differences. `rt_import` calls `PPCInterpreter_virtualHLE` with the trampoline's opcode, which
records the call in the trace and charges the 300 cycles exactly as the interpreter does.

### D5. The function table is a host array indexed by guest address

We use a flat array `native_fn[(ea - 0x02000000) / 4]` over `.text` (`0x02000020`–`0x028F87F4`):
2.35M entries, 19 MB of host memory, outside guest memory.

* This avoids XenonRecomp's in-guest table, which would collide with Wii U memory layout.
* Cemu's own `ppcRecompilerDirectJumpTable` is the same idea.

An indirect call to address `a`:

1. If `a` is in `.text` and has an entry, call it.
2. If `a` is in the trampoline area, take the HLE path (D4).
3. Otherwise log it, stop in debug builds, and fall back to Cemu's interpreter in release builds.

The Phase 1 audit guarantees that every address-taken code location is a function entry, so step
3 should never fire. If it does, that is a boundary bug to fix in `tools/ghidra/`.

### D6. Threads, yielding and stacks use Cemu's fibers

> **Revised 2026-09-29 (owner decision, before M4): exact accounting, per instruction.** The
> reference runs on a virtual clock (M0a): guest time *is* the instruction count, and timeslice
> boundaries, alarms and vsync all hang off it. "Coarse accounting is fine" below predates that:
> native code that counts differently changes thread interleaving, and the trace stops matching
> the reference. So:
>
> * Every generated instruction first does `if (--ctx->remainingCycles < 0) rt_yield(ctx);`, which is
>   the same boundary as Cemu's `while ((--remainingCycles) >= 0)` loop. `rt_yield` clears the
>   reservation and calls `PPCCore_switchToScheduler()` *in place*: the native frames wait on the
>   fiber stack, and the instruction runs once the thread gets its next timeslice. No mid-function
>   re-entry is needed.
> * Import calls charge what the interpreter charges: one cycle for the trampoline instruction
>   (with the same check), then `PPCInterpreter_virtualHLE`'s 300.
> * M4's check stays the strongest one: the whole-route trace equals the reference's
>   (1,124,796,468 calls).
> * Cost: one decrement and branch per guest instruction. Counting per basic block (with a
>   per-instruction checked copy for the block where the quantum runs out) is a later
>   optimisation, held to the same trace.
> * The shipped build runs on the real clock, where interleaving isn't reproducible anyway, so it
>   may switch to coarse counting (below) with a build flag.
>
> Options considered: exact per block from the start (faster, more generator work up front, about
> twice the code), and coarse counting with a relaxed per-thread call-sequence check (weakest
> oracle; guest time drifts from the reference).
>
> **As built (M4).** `RT_TICK(pc)` precedes every generated instruction. `rt_yield` ends the slice
> the way the loop that called the hook would: Cemu's two loops differ, so patch 0011 passes a flag.
> * `__OSFiberThreadEntry` clears the reservation and switches, and the next slice gets the
>   quantum plus the scheduler's jitter.
> * `PPCCore_executeCallbackInternal` switches (`OSYieldThread`) and then resets the budget to the
>   bare quantum.
>
> The runtime keeps a callback depth per guest thread. Import calls and calls into the trampoline
> area tick for the trampoline instruction before `PPCInterpreter_virtualHLE` charges its 300. Diff
> mode checks the accounting per function: a native run starts with an unlimited slice, and the
> cycles it spent must equal the instructions the interpreter executed for the same call. Over the
> whole route, 9,697,829 timeslices end inside native code, and the trace equals the reference's.

Every guest thread already runs on its own Cemu fiber: a ucontext with a 2 MB stack
(`util/Fiber/FiberUnix.cpp`), scheduled over three host threads for the three cores.

* **Native frames live on the fiber stack.** When an HLE call blocks (for example
  `OSWaitEvent`), Cemu switches fibers from inside C++, and the native caller's frames simply stay
  suspended. This is a large simplification over UnleashedRecomp, which needed real host threads
  per guest thread.
* **Preemption.** Cemu preempts on an instruction budget (`remainingCycles`, quantum 45,000). The
  generated code decrements a budget at loop back-edges and before calls. When it goes negative,
  it clears the reservation and calls `PPCCore_switchToScheduler()`. The budget only drives
  fairness: guest time comes from host rdtsc (`PPCTimer.cpp`), and nothing depends on instruction
  counts. Coarse accounting is fine.
* **Stack depth.** Guest recursion becomes host recursion. With `musttail` on every tail call and
  2 MB fibers this should suffice. The fiber size is a single constant if it doesn't, and diff
  mode logs the peak depth.

### D7. Save/restore helpers become synthesised entry functions

The GHS helpers at `0x028F5EE0`–`0x028F626C` are straight runs of `stw`/`stfd`/`psq_st`/`lwz`/`lfd`,
indexed off `r11`, with one entry per register. Callers `bl` into them part-way, and epilogues `b`
into them.

As XenonRecomp does (`Recompiler::Analyse`), the generator emits one function per entry point,
each with its own copy of the rest of the run: `save_gpr_14` … `save_gpr_31`, `rest_fpr_N`, and so
on. Phase 1 already made each called entry a function in `functions.csv`. The generator only has
to allow fallthrough into the next entry, by duplicating code, for this address range.

A later optimisation could inline them at call sites.

### D8. Verification is built in from day one

There are three layers, all using Cemu's interpreter as the oracle, running in the same process.

1. **Instruction fuzzing.** For each of the 157 mnemonics the game uses, take random CPU states and memory
   windows and compare one generated instruction against `PPCInterpreterSlim_executeInstruction`.
   This runs as a unit test on this server with no game data.
2. **Function diff on "pure" functions.** From the call graph, mark functions that reach no
   import and no indirect call. In diff mode, a sampled call of such a function runs natively with
   stores journaled, rewinds memory, runs the interpreter to the same return, and compares all
   registers and memory writes.
   * This is the in-process differential idea from nWiiURecomp's `NWIIU_DIFF`, restricted to
     functions whose side effects can be replayed.
   * Functions that call HLE can't be replayed: an allocation, a thread operation or a GPU submit
     would happen twice.
   * **As built (M3, `src/runtime/diff.cpp`).** The native run comes first, at the call's entry,
     with every store journaled (the old bytes); it is then rewound, memory from the journal and
     registers from a copy. The interpreter runs the call as usual, charging cycles and being
     preempted as the reference is, while the runtime decodes each store it executes. The two
     are compared where the native run left the guest (its final LR, with its final stack
     pointer), which also covers the GHS restore-and-exit helpers that return to their caller's
     caller. Calls that were preempted are counted apart (other threads may change what they
     read). A call that reaches an HLE trampoline would contradict the purity analysis and is
     reported as escaped. Guest-visible behaviour stays the interpreter's, so the OS-call trace
     still equals the reference's while every sampled call is checked on real game state.
3. **End-to-end.** Boot to the title screen and a scripted gameplay path with frame dumps and GX2
   call traces. Compare against the unmodified reference Cemu on the same input script (D16), and
   check the frames by eye as well as by count.

A function is only "done" when layer 2 passes for it. This is the external oracle
`CLAUDE.md` asks for.

### D9. Replacing functions: overrides

* Generated functions are emitted `weak`. A strong definition with the same name in
  `src/overrides/` wins at link time, which is XenonRecomp's mechanism.
* The generated original stays callable as `orig_f_X`, so an override can wrap it.
* An override for a pure function is checked by the same diff harness (D8.2), with the
  interpreter running the original.
* **Enhancements are overrides.** For 60 fps (interpolating camera and actor transforms, as
  setsail does), the overrides are written against names from Phase 2.
* **Mid-function hooks** (XenonRecomp's `[[midasm_hook]]`) are deferred until a real need appears.

### D10. Cemu's boot-time code patches

`GamePatch_scan()` (`Cafe/GamePatch.cpp`) rewrites game code at boot with HLE opcodes, `blr`s and
branch fixes. The generator reads the RPX, not patched memory, so these patches would silently
not apply.

**Task:** list which patches match WWHD. For each, either implement it as an override or confirm
it is not needed. Assert at boot that no patch touched a native-dispatched range.

**Found (M3).** The runtime hashes every function in guest memory against the RPX at boot. Exactly
two differ, both Cemu's TWW patches: `f_027F9994` (the "TWW race condition": four calls to the
mutex lock/unlock wrappers become `nop`s, to avoid a deadlock in single-core mode) and
`f_028137E0` (the US "DSP kill channel" patch at `0x02813878`, `bge` → `b`). Both stay
interpreted, and so would any native function that reaches them.

**As built (M4).** `config/US_v0/code_patches.csv` lists the five words those patches change,
with their original values and the evidence. The generator applies them before generating, so the
generated code is the code Cemu runs, and the boot check finds 0 functions patched in memory. A
patch Cemu applies that the csv lacks would show up there and keep its function interpreted.

Cemu graphic-pack code patches (such as the FPS fix at `0x025AC25C`) are handled the same way:
they become overrides only when wanted.

### D11. Build and toolchain

* **The recompiler (generator) is Python**, in `tools/recomp/`. It shares the RPX, CSV and
  relocation code already in `tools/`. 2.35M instructions is a one-off, seconds-to-minutes job.
  Rewrite it in C++ only if its speed ever matters.
* **Generated code and the runtime are C++20, built with clang.** clang is needed for
  `[[clang::musttail]]`, and XenonRecomp requires it too. The server has only GCC 14, so clang
  gets installed.
* **Required flags:** `-fno-strict-aliasing`, plus strict floating point (XenonRecomp uses
  `-ffp-model=strict`). Espresso rounds single-precision ops to single, and `fres`/`frsqrte` use
  Espresso's estimate tables, which Cemu has.
* **Cemu stays close to unmodified,** as a git submodule at a pinned commit plus a small patch
  series:
  * the execution seam: two call sites and an export of the HLE table lookup;
  * a build option that leaves out `HW/Latte`, `OS/libs/gx2` and `OS/libs/TCL`;
  * a frontend that implements `WindowSystem`.
  * Cemu's `CemuCafe` target already has no wxWidgets includes. It needs Config, Input, Audio
    and Util at link time (`src/CMakeLists.txt:209`).
  * The frontend is SDL3, replacing `gui/wxgui` and `main.cpp`.
* **Headless on this server:**
  * CPU and logic milestones run with GX2's null backend (D12), with no rendering at all.
  * Rendering tests use Vulkan through Mesa's lavapipe (CPU software rendering), for both our
    build and the reference Cemu.
  * Real play needs a GPU machine.
* **Graphics libraries:**
  * AMD's **addrlib** (MIT) for surface tiling and size calculation. It is what real GX2 used, and
    decaf-emu vendors it too.
  * **shaderc/glslang** for SPIR-V (already installed here).
  * **Vulkan** via volk + VMA.
* **Licences:** Cemu files stay MPL-2.0 (file-level copyleft), which is compatible with any
  licence for our own code. **We copy no nWiiURecomp code**: its licence is unclear.

### D12. GX2 is our own native module; Latte is never built

> **Revised 2026-09-29 (M0b): we keep Cemu's gx2 and TCL as the front half and replace Latte,
> the consumer.** What we found:
>
> * **Cemu's gx2 is what matches the reference.** It writes some Cemu-only packets (`IT_HLE_*`, for
>   clears, surface copies, swaps and timers) into command buffers *and display lists*. The game
>   sees display-list sizes (`GX2EndDisplayList`), so a front half written to real-GX2 semantics
>   would diverge from the reference. WWHD records about 24k display lists and calls 113k in its
>   first 600 frames.
> * **Everything guest-visible happens in the command stream.** That covers display lists,
>   context-state shadowing (register writes mirrored into guest memory) and `LOAD_*` packets, as
>   well as fences, timestamps, bottom-of-pipe callbacks and swap/flip bookkeeping. One command
>   processor handles all of them uniformly.
> * **The command processor's register file is the "decoded register-level state"** the backend
>   needs. So the backend does consume PM4 after all, but only the command processor parses it,
>   and renderers read registers.
>
> `src/gpu/null_gpu.cpp` is that command processor with nothing behind it. Linked without Latte
> (`wwhd-null`), it reproduces the reference's OS-call trace exactly (M0b status below). The
> Vulkan backend (D13) will be a renderer on the same command processor. The front-half list below
> still describes what must stay exact; it now holds by construction.

WWHD imports 108 GX2 functions. We register our own `gx2` module in Cemu's HLE table in place of
Cemu's (`Cafe/OS/libs/gx2`, 8k LOC), so import resolution (D4) lands in our code. The module has
two halves.

**The front half is exact, and shared by every backend.** Everything the game can observe must
match real GX2, because the game stores or computes with these results:

* surface size and alignment (`GX2CalcSurfaceSizeAndAlignment`, via addrlib), and the
  `GX2Calc*`/`GX2Get*` sizes and GPR counts;
* the register values that `GX2Init*Regs` writes into guest structs (color and depth buffers,
  textures, samplers);
* context-state blocks (`GX2SetupContextStateEx`, `GX2SetContextState`,
  `GX2GetContextStateDisplayList`);
* display-list bookkeeping (`GX2BeginDisplayListEx`, `GX2EndDisplayList`, `GX2CallDisplayList`,
  `GX2DirectCallDisplayList`, `GX2GetCurrentDisplayList`);
* vsync and swap (`GX2SetSwapInterval`, which WWHD sets to 2 (30 fps), `GX2WaitForVsync`,
  `GX2GetSwapStatus`, `GX2SwapScanBuffers`);
* GPU timestamps (`GX2Sample*GPUCycle`, `GX2GPUTimeToCPUTime`, from host timers).

Cemu's GX2 and decaf-emu are the references for exact values. Differences are caught by the
comparison in D16.

**The backend** turns state and draws into Vulkan (D13). There is also a **null backend** that
tracks state but draws nothing. It lets the CPU track (M-milestones) boot the game headless on
this server before rendering exists.

**The state model.**

* GX2 is a thin layer over GPU registers: the `Set*`/`Set*Reg` calls carry register values.
  Our state tracker keeps a *decoded* register-level state (blend, depth, raster, targets, shader
  bindings, uniform blocks, samplers, textures, attribute buffers, viewport and scissor, and so
  on). It maps that to Vulkan pipeline state, with a pipeline cache keyed on the decoded state.
* Understanding register *semantics* is unavoidable, but nothing parses PM4 streams.
* Display lists are recorded as **host command lists keyed by the guest buffer address**. A
  call to an address that wasn't recorded by our `BeginDisplayList` is a hard error. That catches
  any pre-built display lists in game data; G0 checks there are none.

**Cemu coupling.** Eight Cemu OS files reference Latte. The main ones:

* `LatteBufferCache_notifyDCFlush` (14 uses) comes from the game's cache flushes. It is exactly
  the signal we need for "the CPU wrote GPU data".
* `LatteGPUState` fields (the shared area, OSScreen, gamma).
* `Latte_Start`/`Latte_Stop`.

Our GX2 module provides these few symbols as a shim, so those files link unchanged.

### D13. Vulkan backend

* **Surfaces.** A color or depth buffer, texture or scan buffer in guest memory maps to a host
  `VkImage`, keyed by (address, format, dims, tile mode, swizzle):
  * tiled layouts are untiled with addrlib;
  * formats with no Vulkan equivalent are converted; BCn formats map directly.
* **Coherency (CPU writes).** Buffers and textures live in guest memory, and the CPU writes vertex
  and uniform data every frame. The game must call `GX2Invalidate` and `DCFlushRange` before the
  GPU reads, because the real hardware has caches. So:
  * those calls are our upload points, with dirty-range tracking;
  * uniform blocks and attribute buffers are copied at draw time until profiling says otherwise.
* **Coherency (GPU writes the CPU reads).** Examples: `GX2CopySurface` into CPU-visible memory,
  and the Pictograph Box photos. These are written back at `GX2DrawDone` or on explicit
  invalidation.
* **Presentation: TV only.**
  * `GX2CopyColorBufferToScanBuffer` for the TV target and `GX2SwapScanBuffers` present the TV
    image (1920×1080 / 1280×720) in a single window.
  * GamePad (DRC) targets are no-ops in the backend: `GX2SetDRCBuffer`, copies to the DRC scan
    buffer, `GX2SetDRCEnable`. The front half still returns exact `GX2CalcDRCSize` and related
    values, because the game allocates memory from them.
  * If the G0 trace (in Pro-Controller mode) shows the game still rendering a GamePad view, an
    override can skip that pass later; it is wasted work, not a correctness issue.
  * Pacing follows the game's swap interval. The 60 fps enhancement is D9 plus interpolation,
    not a pacing change.
* **Features to confirm with the G0 trace before building them:**
  * geometry shaders (GX2 uses memory ring buffers, which don't map directly to Vulkan geometry
    shaders);
  * `GX2ExpandAAColorBuffer` / MSAA;
  * HiZ;
  * stream-out.

  *G0 answer (scripted route, D15):* none of the four occurs. The backend needs no geometry
  shaders, MSAA, HiZ or stream-out for this route. It does need quad lists (273k draws), a
  depth-only pass (a 1024×1024 D16 shadow map), float render targets (R11G11B10) and GX2's
  GPU-side surface copies.

*As built for G2 (`src/gpu/vk`):* a renderer on the null GPU's command processor. It runs on the
GPU thread and never writes guest memory, so the trace is the same with it on or off.
* **Draws** follow Cemu's Vulkan renderer: decompiler GLSL compiled with glslang, one descriptor
  set per stage, dynamic uniform buffers for the uniform variables and blocks, raw vertex formats
  decoded in the shader, pipeline state from the registers, dynamic rendering. Uniform, vertex and
  index data are copied from guest memory at every draw.
* **Surfaces** are keyed by (address, GX2 format): color and depth targets, created on first clear
  or draw and grown as needed. Scan-out copies the TV target, as the reference's capture does.
* **Textures** sample a surface when one was drawn at their address. Otherwise they are untiled
  and decoded from guest memory by Cemu's texture loader (addrlib and its `TextureDecoder`s, kept
  in `wwhd-null`), in the formats Cemu's Vulkan renderer picks, and reloaded when a per-frame hash
  of their data changes. See the G2 status for copies and mip chains.
* **Surfaces** also have array layers (a target attaches the slice `CB_COLORn_VIEW`/`DB_DEPTH_VIEW`
  selects; clears clear their slices). Guest memory is shared by all of them but each has its own
  image, so a surface overwritten by a later write to overlapping memory is reset to zero at the
  next swap, as the reference's texture cache does lazily (G2 status).
* **GX2's GPU-side surface copies** (`IT_HLE_COPY_SURFACE_NEW`) don't occur on the route; the
  renderer only reports them (`WWHD_RENDER_COPIES`). The CPU copies are done by the null GPU.

### D14. Shaders are recompiled ahead of time, like the CPU code

The Wii U has no runtime shader compiler, so every shader the game can bind is in its files:

* `Common/Shaders/{particle,Prim}/wii_pipeline.sharcfb` and `render_buffer.sharcfb`, the
  GameCube-pipeline variants;
* the `.sharcfb` archives inside `Cafe/Common/agl_resource_cafe.sarc` (Nintendo's agl);
* two `.gsh` files (the primitive renderer);
* possibly more inside the `.pack`/`.szs` archives, which the extractor must search.

**The pipeline** (`tools/shaders/`, run at build time on the user's machine):

1. Unpack Yaz0/SARC/SHARCFB/GSH and collect every GX2 vertex, pixel and geometry program, with
   its register header. Programs are keyed by a hash of their microcode.
2. Translate the R600 microcode to SPIR-V.

**The translator.** This is the one large component we write, a "LatteRecomp" in the role
XenosRecomp plays for Unleashed.

* Cemu's `LegacyShaderDecompiler` (MPL-2.0, about 14k LOC, proven on this very game) is the
  starting point, restructured as an offline tool.
* It emits GLSL, which glslang turns into SPIR-V.
* decaf-emu's SPIR-V translator is a second reference.

**State that isn't in the microcode** (the vertex fetch layout from the fetch shader,
render-target formats, alpha test, and so on) becomes **specialisation constants** or a small,
enumerated set of variants that the G0 trace confirms. It never means compiling at runtime.

**As built (G1, `tools/shaders/`).** Cemu's decompiler is not restructured yet: the translator links
it and glslang the way Cemu's Vulkan renderer runs them, like the M1 fuzzer, and emits GLSL and
SPIR-V. The shaders are in more places than the list above suggests:
* 2,794 SHARCFB archives (agl's format, version 9, little-endian), mostly inside the per-stage and
  per-object `.szs` archives. HD models are BFRES files with their own shader archives.
* 2 GFD files (`.gsh`), and 2 more embedded in `Common/Misc/Misc.bfres`.

Programs are keyed by the FNV-1a hash of their microcode. The draw state not in the microcode
comes in two ways (tools/shaders/README.md):
* from a run's dump of each variant's register file (`WWHD_GPU_DUMP`), which gives exactly the
  reference's inputs;
* or chosen neutral for the whole corpus: 2D textures except where the microcode sets a cube-map
  index, float targets, and a float4 fetch layout.

**The runtime** loads SPIR-V by microcode hash.

* A shader missing from the corpus is a bug: it is logged, and in development builds it is
  translated on the fly as a fallback.
* Like the CPU code, **the SPIR-V is generated on the user's machine and never distributed**.

### D15. Where the graphics work starts: the G0 trace

Before building the backend we measure what WWHD actually does:

* Run the reference Cemu with GX2 call logging on a scripted route: boot, title, file select,
  Outset Island, sailing, a dungeon room, the menus, and the Pictograph Box.
* Record which of the 108 functions are called, with what arguments and formats; which shader
  programs are used; and whether geometry shaders, MSAA, HiZ or stream-out appear.

The G milestones are scoped from that trace, not from the import list.

**First look (2026-09-28, reference Cemu 2.6, boot to title, about 3.5 minutes):**

* 76 of the 108 imported GX2 functions are called.
* The hottest are `GX2SetVertexUniformBlock` (3.1M calls), `GX2DrawIndexedEx` (2.5M) and
  `GX2SetBlendControl` (1.8M).
* **Display lists are central:** 27.7k `GX2BeginDisplayListEx`, 212k `GX2CallDisplayList` and
  11k `GX2DirectCallDisplayList`. Record-and-replay (D12) is on the hot path from the first frame.
* **Cemu's text logging is unusable at this scale:** 956 MB in 3.5 minutes. The G0 tracer has
  to be a compact binary log, written by a small hook in the reference's source build: call id,
  arguments, and hashes of referenced buffers. This is part of the reference Cemu patch, together
  with the deterministic clock.

**G0 (2026-09-29, the scripted route to f10800: boot, title, file select, name entry, the intro,
Aryll's dialogue, gameplay on Outset).** Two tools: `tools/reference/g0_gx2.py` reads the OS-call
trace (every GX2 call with its register arguments), and `WWHD_GPU_STATS` makes the null GPU record
the register state at every draw. Findings:

* **Calls:** 85 of the 108 imported GX2 functions are called. The other 23 include every geometry
  shader function, `GX2ExpandAAColorBuffer`, `GX2ExpandDepthBuffer`, `GX2InitDepthBufferHiZEnable`,
  `GX2SetDRCEnable`/`GX2SetTVEnable`, vertex textures and samplers, point size and line width.
  Stream-out and occlusion queries aren't imported at all. `GX2SetShaderModeEx` is only ever mode 0
  or 1, never the geometry-shader mode. The swap interval is 2.
* **Draws:** 7,023,899 (7,020,305 indexed, 3,594 auto). Primitive types are triangle lists (4.59M),
  strips (2.16M) and quad lists (273k). There is always one instance.
* **At the register level on every draw:** geometry shaders off, stream-out off, one sample (no
  MSAA). About 1.0M draws have colour writes disabled (depth only).
* **Shaders:** 64 fetch, 236 vertex, 0 geometry and 268 pixel programs by content, forming 281
  pipelines and **287 variants** (pipeline × render-target formats × MSAA). Open question 5 is
  answered: few enough to enumerate.
* **Targets** (all tile mode 4, 2D tiled thin, which addrlib untiles):
  * the scene at 1920×1088 (RGB10A2 and R8, D32F depth);
  * a half-resolution 960×544 chain;
  * R11G11B10-float downsamples from 480×272 down to 64×48;
  * 1024×544 R8;
  * a 1024×1024 D16 shadow map.
* **The GamePad view is still rendered** in Pro-Controller mode: an 864×480 target with its own
  depth buffer, about 7 draws per frame (78,836 in all). Every frame copies both a 1920×1080
  buffer to the TV and an 854×480 buffer to the DRC. It's cheap, and an override can skip it (D13).
* **HiZ:** the game sizes HiZ buffers (`GX2CalcDepthBufferHiZInfo`, 9 calls), but Cemu's gx2 encodes
  no HiZ in the command stream: it writes the depth image's address into `DB_HTILE_DATA_BASE` and 0
  into `DB_DEPTH_BASE`. A backend on this register file (D12) inherits that Cemu dialect. HiZ only
  affects performance.
* **Copies:** `GX2CopySurface` runs 50 times, all at boot. The null GPU ignores them (nothing is
  drawn), but a real backend has to perform them.

Not covered yet: sailing, a dungeon room, the menus and the Pictograph Box. The same two tools
rerun unchanged once the route reaches them.

### D16. Graphics verification

* **The reference** is upstream Cemu, run under Xvfb with lavapipe, on the same scripted input.
  Its patches (tools/reference/README.md) make it deterministic and observable, plus one rendering
  fix where it disagreed with the console's shared memory (0013: a mip chain allocated below its
  base level); a fix goes in only with evidence that the console would draw it that way.
* **Determinism.** Both builds run with a test-mode clock (`OSGetTime`/`OSGetSystemTime` advance
  a fixed step per frame) and frame-indexed input. The same frame number then shows the same
  scene. The reference Cemu needs patches for that (0001-0006).
* **Three comparisons, strictest first:**
  1. **GX2 call streams** must match exactly (calls, arguments, and hashes of referenced
     buffers). A mismatch here is a CPU or front-half bug, not a rendering one.
  2. **Front-half outputs** (the structs `Init*Regs` writes, `Calc*` results) must be
     byte-identical.
  3. **Frames** match within a tolerance (SSIM), with both renderers on lavapipe. Every
     difference above tolerance is logged, and the pair of frames is kept for review.

### D17. Input: Pro Controller through padscore; GamePad absent

* **`VPADRead` reports no controller** (`VPAD_READ_ERR_NO_CONTROLLER`). The other VPAD imports
  (motor, touch calibration, headphone status) are harmless no-ops.
* **The Pro Controller is served through `padscore`:** `KPADInitEx`, `KPADReadEx` (Pro Controller
  extension data), `WPAD*` status. The host side maps SDL3 gamepads onto the Pro Controller
  layout: buttons, both sticks, ZL/ZR. This is the main input seam, and where remapping lives.
* **The reference runs the same way:** a Pro Controller profile, no GamePad window. Its scripted
  route input goes through the same KPAD path, so reference traces follow the code the recomp
  will actually run.
* **Software keyboard (name entry):** WWHD enters the player's name through the system
  keyboard (`nn_swkbd`). Cemu implements it with an ImGui overlay drawn by its renderer, and that
  renderer isn't in our product. The recomp needs its own: at minimum a text prompt in our
  frontend feeding `SwkbdGetInputFormString`, later an in-game-styled keyboard. The reference
  answers it deterministically (`CEMU_SWKBD_AUTO`).
* **Later, second screen:** our own feature (e.g. map/inventory on a handheld's second display)
  built as overrides that draw extra views, not the DRC path.

### D18. The shipped game does not depend on Cemu; Cemu stays the reference

*Decided 2026-09-30.* The product will run on its own runtime; Cemu remains, indefinitely, the
development-only reference that traces, captures and texture dumps are checked against.

* **Why.** Portability (Cemu's runtime assumes a reserved 4 GiB guest region, per-platform fibers,
  x86 habits in places and a large dependency tree), timing freedom (native code still counts every
  instruction to reproduce Cemu's emulated three-core scheduler; a game-specific runtime can use host
  threads and real-time pacing, which 60 fps needs), and size: the game needs a small, known part of
  an operating system, not an emulator.
* **The surface.** Along both scripted routes the game calls about 245 OS functions of the 426 it
  imports: `coreinit` 87 (1.2 billion calls, nearly all message queues), `gx2` 85 (107 million),
  `snd_core` 28, and about 45 more that are mostly trivial (`nn_boss`, `nn_olv`, `nn_act`, `nn_ac`,
  `nn_save`, `swkbd`, `erreula`, `padscore`, `vpad`, `proc_ui`, `nlibcurl`, `nsysnet`). Behind them
  sit the scheduler, heaps, the file system and save services, and the AX mixer
  (`tools/reference/hle_trace.py summary` on the route traces).
* **Vendor what is portable, rewrite what is emulator-shaped.** Self-contained C++ that is already
  portable (the shader decompiler, texture decoders, the address library, the fetch-shader parser)
  moves into this tree under MPL-2.0 with attribution. The scheduler and threads, memory model,
  loader, file system and save services, audio output and the HLE calling convention are rewritten.
* **The seam is the HLE table, or the object file.** Our OS layer (`src/os`) implements imports
  and takes over their entries in Cemu's HLE handler table at boot, so native code and the interpreter both reach it
  while Cemu's dispatch still writes the trace and charges the 300 guest cycles (patch 0002). Every
  function not yet ours falls back to Cemu's. `WWHD_OS=cemu` restores all of Cemu's, for comparison.
  The table and `PPCInterpreter_t` stay the calling convention until the scheduler is ours.
  Libraries Cemu's own code calls into (the scheduler drives snd_core; CafeSystem and coreinit
  call gx2's core) are forked instead: our copy of Cemu's source, under Cemu's names, linked in
  place of Cemu's object at its position in the link, so its guest-memory slots keep their
  addresses (added 2026-09-30, for snd_core).
* **Order, least coupled first:** leaf functions (memory, cache operations) and service stubs;
  the platform shell (own CMake build, SDL3 window, input and audio device, which is also M5); the
  small libraries (input, save, keyboard and error screens); `gx2`'s front half; audio; `coreinit`
  with the scheduler; the loader and memory map.
* **Verification: the scheduler has two modes.** Traces are only comparable while scheduling is
  identical, so our scheduler gets a deterministic mode that follows Cemu's rules on the virtual
  clock (the one every check uses) and a real-time mode on host threads for players. Each replaced
  function must keep the whole-route traces identical.
* **Status (2026-09-30):** 255 functions are ours at the HLE table (coreinit 31, gx2 179, nn_ac 2,
  nn_act 2, padscore 6, vpad 4, erreula 15, swkbd 16), and 22 of Cemu's source files are forked
  (`src/forks.txt`): snd_core whole (its sound, hashed block by block, equals Cemu's on both
  routes), the scheduler (coreinit's threads, scheduler, alarms, message and thread queues,
  spinlocks, synchronization, callbacks; the Espresso timeslices and the timer with the virtual
  clock), gx2's core (GX2.cpp, command pool, events, GX2Init) and proc_ui. Both route traces, GPU
  command streams and sound are identical to Cemu's. The forks are Cemu's code, unchanged so far:
  ours to change, and where the scheduler's deterministic and real-time modes get built. gx2's front half
  is Cemu's code ported into `src/os/gx2` (`tools/gx2_port.py`, file by file): every state setter,
  shaders, textures and samplers, surfaces, render targets, context states, draws, clears, copies,
  GX2R resources, still writing through Cemu's command pipe; 96.3% of the route's 82 million gx2
  calls. Its check is stronger than the trace: the GPU command stream, hashed per frame, must equal
  what Cemu's gx2 sent (`tools/reference/stream_check.sh`). gx2's core stays Cemu's for now (14 of
  the route's functions: the command pool and display lists, flush, GPU timestamps, GX2DrawDone,
  vsync waits, swap and GX2Init): it keeps guest OS objects in Cemu's memory (thread queues, an
  event-callback thread, a semaphore, the pool state) and waits through the scheduler, so it moves
  with the scheduler, and is where D18's rewrite of submission and synchronisation happens. Libraries the game loads itself
  (swkbd, erreula) are taken over right after `OSDynLoad_Acquire` loads them. Input is a whole
  library: the input script (for routes) or the window's keyboard and gamepad (SDL3) drive one Pro
  Controller, and the GamePad is absent. The software keyboard and error dialogs are ours, drawn by
  the frontend over the game; the name-entry route replays through them. The platform shell has
  its window, input and sound on SDL3 (Linux: X11 or Wayland), presented by our renderer; windowed,
  the save route's trace is still the reference's. What our functions still borrow from Cemu goes
  through accessors in `src/os/os.h`: the clock, the current thread, sleeping, guest callbacks,
  gx2's swap count and the system area. Imports Cemu never implemented (8 in padscore,
  `VPADBASEGetHeadphoneStatus`) have no HLE entry to take over and wait for our loader.
  Held back until what they stand on is ours:
  - with the scheduler: `proc_ui` (threads, events and rendezvous of its own, the system message
    queue);
  - with the file system: `nn_save` (every call an asynchronous FS request waited on through the
    scheduler);
  - with their whole library: `nn_act.Initialize` (loads accounts for the rest of nn_act),
    `nn_boss` and `nn_olv` (objects with vtables in Cemu's memory, IPC to Cemu's IOSU), `nlibcurl`
    and `nsysnet` (setup the rest of the library checks), and `OSGetSystemInfo` (returns a
    structure in Cemu's memory).

### Profile of the native build (2026-09-30)

To design the scheduler's real-time mode on numbers: `WWHD_PROFILE=path` samples host CPU time
(`src/runtime/profile.cpp`, `tools/profile_report.py`). The save route, native, headless, the trace
filtered down to nothing (`CEMU_HLE_TRACE_FILTER=zzz.`), on the worker:

* **Speed.** 1800 frames (60 s of game time) in 44 s: 1.36x real time with all three emulated
  cores on one host thread. With the full trace written it takes 74 s: the trace is 40% of a
  traced run.
* **Threads.** The null GPU thread uses a whole host core (36.3 s of CPU, 95.6% in `sched_yield`:
  it spins waiting for commands). The CPU thread (`OSSched[core=0]`) uses 36.0 s. Everything else
  is under 2 s.
* **The CPU thread** (samples, libc time counted for its caller): guest code 67%, its helpers
  (FP compare, paired singles) 3.6%, runtime 3%; message queues 9.6%; the trace recorder 8.7%
  (it locks a mutex on every OS call even when tracing is off); guest thread switches 2.8%; HLE
  dispatch 1.8%; gx2 1.5%; the rest of the scheduler 1.4%; snd_core 0.6%.
* **The message queues** are one guest thread pushing to and popping from its own queue: 68.9
  million `OSSendMessage` (non-blocking) and `OSReceiveMessage` (blocking, never blocks) pairs
  on the save route, 38,700 per frame, 80% of all OS calls. Both come from thin queue wrappers
  (`f_02760338` push, `f_02760374` pop) in unnamed library code (0x0275xxxx-0x0281xxxx). Each
  op is a full OS call: HLE dispatch, the trace recorder's mutex and Cemu's scheduler lock (a
  recursive pthread mutex).

**After the quick wins** (same day; `tools/reference/timing.sh`, no trace at all, `WWHD_EXIT_FRAME`
ends the run): the GPU thread sleeps until the CPU submits, and the CPU sleeps until a submission
retires under the virtual clock (both used to spin; `src/os/tcl`, forked, notifies); the trace
recorder no longer runs, or locks, when there is no trace. The save route takes 37.5 s from launch,
1.60x real time (44 s before, with the trace filtered down to nothing, the fastest the old build
could end a route); the whole route 155 s, 2.32x. The GPU thread's CPU time went from 36.3 s to
0.7 s on the save route. On the CPU thread guest code is now 74%, message queues 9.3% (of it the
scheduler lock, a recursive pthread mutex, 3%), guest-code helpers 4.5%, runtime 3.5%, thread
switches 2.5%, HLE dispatch 2.5%. A cheaper scheduler lock would save at most ~2% and needs the
same per-thread owner check (the lock is held across fiber switches), so it waits for the
real-time scheduler's own locking. Traces, command streams and sound stay identical.

What it means for the real-time scheduler: the GPU thread must wait instead of spinning; the
per-call cost of uncontended OS calls (trace mutex, scheduler lock) matters more than thread
switches, and with one host thread per core the global scheduler lock would be contended 77,000
times a frame, so queues and mutexes need a cheap uncontended path; guest code itself is 70% of
the CPU thread, where per-block cycle counting (D6) and register caching (D2) are the lever.

### D19. The scheduler: exact for checks, real time for play

*Proposed 2026-09-30, from the profile above and the save route's trace; open points at the end.*
The scheduler is Cemu's, forked (D18): guest threads are fibers, three emulated cores, a 45,000-cycle
timeslice (plus jitter), one global scheduler lock (a recursive pthread mutex), and two host layouts:
one host thread running the three cores in turn, or one host thread per core (Cemu's "multicore
recompiler" mode). Guest time is host rdtsc, or, with the virtual clock, the instructions executed.
Every check runs the virtual clock on one host thread, and so does play today.

**What the game does** (save route, virtual clock, `sched.slice` records):
* All three cores are busy all the time: the idle jumps add up to 0.0% of guest time.
* Core 1: one thread (1603f4a8) holds 68% of the core in 1.1 million timeslices; it is the thread
  of the 69 million message-queue pairs (the profile above). It pushes a message to its own queue
  and then waits for that message (`f_0275FFCC` pops until the expected value arrives, reached
  through a function pointer after `OSSetThreadSpecific`): a coroutine-style task switch built on
  OS queues, which, when it switches to the running task, costs two OS calls and changes nothing.
  Another thread (0e074ec0) has 30%.
* Core 2: one thread (0e005f40) 72%, with 16.5% and 9.2% for two others. Core 0: one thread
  (104b7818) 62%, then 15%, 12% and smaller ones. Two threads move between cores.
* One host thread runs all this at 1.60x real time on the save route and 2.32x on the whole route
  (`timing.sh`), 74% of it in guest code.

**Requirements.**
1. *Checks stay exact.* The deterministic mode is today's behaviour, bit for bit: virtual clock,
   one host thread, per-instruction accounting (D6). Any change to shared scheduler code keeps
   traces, GPU command streams and sound identical (`stream_check.sh`).
2. *Play runs on real time without waste.* Guest time follows the host clock; frames are paced by
   the display's vsync and sound by the audio device; no host thread spins: an idle core sleeps
   until a thread becomes runnable, an alarm is due or vsync comes (today the single-thread idle
   loop polls `__OSCheckSystemEvents` without sleeping, which burns a host core whenever the game
   waits). Enough headroom for a steady 30 fps, and later 60 fps interpolation (M6).
3. *Portable.* No x86-only pieces: `std::chrono::steady_clock` (or the platform's counter) instead
   of rdtsc; a small context switch per ABI (x86-64 System V and Windows, arm64) instead of
   ucontext, whose `swapcontext` makes a signal-mask system call on every switch (2.5% of the CPU
   thread) and is deprecated on macOS.
4. *Measured.* `timing.sh` and the profiler for cost; for real time, frame-time percentiles and
   per-thread CPU, and the scripted routes still play (their input is keyed on the swap count, not
   on time) to the same end scene.

**Design.**
* **One scheduler, two modes**, chosen at start: *deterministic* (the virtual clock, what every
  check uses) and *real-time* (the product's default). They share the run queues, thread states,
  the guest-visible structures and the OS functions; they differ in where time comes from, how an
  idle core waits, and cycle accounting (exact per instruction, or per basic block once that is
  exact too, item 2 below; real time may count coarsely).
* **Real time starts on one host thread**, as today: it already runs 1.6x real time, and one host
  thread keeps the single scheduler lock uncontended. Three host threads (one per core) come
  second, only if heavier routes need them: they turn 77,000 scheduler-lock acquisitions a frame
  into contention, so they need per-object locks for queues, mutexes and events, with the global
  lock only on the slow path (a thread blocks or wakes), and a per-core run queue.
* **Waiting**: an idle core sleeps on a condition variable (woken by `__OSMakeRunnable`, an alarm
  deadline or vsync); the GPU thread sleeps until work is submitted (done); the audio frame cadence
  comes from the device's demand, as Cemu's real-time path does.
* **Vsync** comes from presentation (FIFO) when there is a window, else from a host timer at 60 Hz.
* **Fibers stay** (native frames live on a guest thread's fiber stack, D6), with our own context
  switch in place of ucontext.
* **Hot OS patterns** get real-time-only fast paths through overrides (D9), each with an argument
  that the guest can't tell: first the switch-to-self (push to the own queue with no other waiter,
  then wait for that same message), 9% of the CPU thread. They change the OS-call sequence, so the
  deterministic mode never uses them.

**Order**: (1) exact per-block cycle counting (D6's planned optimisation, held to the trace);
(2) real time on one host thread: host clock, sleeping idle, vsync from presentation; (3) our
context switch; (4) heavier routes (sailing, a dungeon, Windfall) timed, to decide on three host
threads; (5) fast paths.

**Open**: one host thread or three (item 4 decides); real vsync needs the GPU machine (open question
8); whether Cemu's three-thread mode has known behaviour differences for this game.

## Milestones

There are two tracks. They meet at M4.

**Foundation**

| # | Milestone | Done when |
|---|---|---|
| M0a ✅ (core) | Reference | Upstream Cemu builds here and boots WWHD under Xvfb + lavapipe. The deterministic-clock patch and GX2 call logging produce frames and traces for the scripted route. |
| M0b ✅ | Runtime skeleton | Our frontend links Cemu's OS libraries **without Latte** (gx2 and TCL stay, see D12), plus the null GPU. WWHD boots with Cemu's **interpreter**. Its GX2 call stream to the title screen matches the reference (D16.1). |

**CPU track**

| # | Milestone | Done when |
|---|---|---|
| M1 ✅ | Instruction semantics | The generator emits all 157 mnemonics the game uses. The instruction fuzzer passes against Cemu's interpreter. |
| M2 ✅ | Whole program compiles | All 39,705 functions generate with zero unknown instructions and zero unresolved branches, and compile with clang. |
| M3 ✅ | Pure functions native | `native_dispatch` is on for pure functions, with sampled diff mode clean over the scripted route (null backend). |
| M4 ✅ | Everything native | HLE calls direct, yield points in place, `GamePatch` handled. The GX2 call stream still matches the reference, and the interpreter fallback counter is 0. |

**Graphics track**

| # | Milestone | Done when |
|---|---|---|
| G0 ✅ (route) | Trace | The D15 trace scopes the backend. |
| G1 ✅ | Shader corpus | Every program in the game files is extracted and translated to SPIR-V that passes `spirv-val`, and every program seen in the G0 trace is in the corpus. |
| G2 ✅ (title) | First pixels | The title screen (TV) renders within tolerance of the reference on lavapipe. |
| G3 | The route | Every scene on the scripted route is within tolerance. |

**Together**

| # | Milestone | Done when |
|---|---|---|
| M5 | Playable | The native CPU plus the native GX2 on a GPU machine play through the scripted route and beyond at full speed. |
| M6 | First enhancement | 60 fps interpolation as overrides (needs Phase 2 names). |

**M0a status (2026-09-29):** done. The patched reference is deterministic along a scripted route:
two fresh boots playing `tools/reference/routes/title-to-game.txt` (title, file select, name
entry, the legend intro, Aryll's dialogue, into gameplay on Outset) give 1,124,796,468 identical
OS-call and scheduler records over 10,800 frames. Frames from the two runs differ only in a few
hundred edge pixels (PSNR 73-75 dB): host rasteriser noise from llvmpipe, which D16.3's
tolerance absorbs. Still open: hardware rendering in the worker.

**M1 status (2026-09-29):** done. `tools/recomp/census.py` decodes all 2,351,474 instructions in
`functions.csv` into 157 base mnemonics (none undecodable). The game uses no OE forms, no FP
record forms, no absolute branches, and SPRs only LR, CTR and UGQR2-5. `tools/recomp/emit.py`
emits all of them, and the fuzzer (`tools/recomp/fuzz/`) matches Cemu's interpreter on every
register and memory byte for the 154 that compute (about 1.8M randomized runs); `dcbf`/`dcbst`
and `tw` are runtime hooks. Two findings: a signed-overflow UB in a naive `neg` that clang
exploited, and NaN payloads, which depend on x86 operand order (see open question 4).

**M2 status (2026-09-29):** done. `tools/recomp/generate.py` emits all functions in 13 s, and
`tools/recomp/build.sh` compiles them on the worker in about 4 minutes with 8 jobs. Totals:
39,720 functions (39,705 from `functions.csv`, the `.syscall` stub, and 14 synthesised GHS
restore entries per D7), 426 imports, 156 shards of 256 functions. There are 0 generator errors
(no unknown instructions, no unresolved branches), and clang builds it with 0 errors and 0
warnings at `-O2`, `musttail` included, into 79 MB of objects. Things the generator had to know:

* branches to imports are `REL24` relocations into `.fimport_*`, 3,643 sites;
* 58 immediates are relocated against data imports (`.dimport_*`) and read from the runtime;
* two weak calls resolve to address 0 and become runtime errors;
* 9 `bcl` are conditional calls;
* the jump-table `bctr`s become a `switch` on CTR.

Not yet in: the yield budget (D6). The runtime behind `rt_*` came with M3.

**G1 status (2026-09-29):** done.
* **Corpus:** `tools/shaders/corpus.py` finds 99,006 programs in the game's files: 30,011 distinct,
  11,765 vertex and 18,246 pixel, with no geometry shaders.
* **Translation:** all 30,011 translate to SPIR-V that passes `spirv-val` (Vulkan 1.1), in about a
  minute on the worker's 8 cores. 29 pixel shaders needed the cube-map dimension their own
  microcode implies.
* **Against G0:** every program the scripted route uses is in the corpus by hash (236 vertex, 268
  pixel). Its 287 variants translate with their dumped runtime state: 574 modules, all valid.
* **Fetch shaders** (64 on the route) are not in files: GX2 builds them at runtime from the
  attribute layout. They are state, decoded into each vertex shader's GLSL.
* **What `spirv-val` proves:** well-formed SPIR-V, not that it renders what the reference renders.
  That is G2's check.

**G2 status (2026-09-29):** done on the route to the title screen. `WWHD_RENDER=vk` turns on a Vulkan
renderer inside `wwhd-null` (`src/gpu/vk`, D13 as built), fed by the null GPU's register file. On
lavapipe along the route to f600 it runs 929,455 draws (0 skipped), 547 shader variants and 397
pipelines. Against the reference's captures every 30 frames (`tools/reference/compare_frames.py`):

* f30-f480, 16 frames: 6 identical, 10 at 94.9-116 dB (no pixel more than 3 levels off);
* f510-f600 at 60.7-70.9 dB, 0-0.01% of pixels off by more than 16: faint speckle on the island's
  outlines, which is the reference's own lazy texture resets (below);
* tolerance: `compare_frames.py --threshold 60` (references against each other are at 73-75 dB).

The OS-call trace with rendering on equals `det-null/a.zst` (59,531,239 calls to f600).

**What it took.** The tools were two dumps at the same swap, the reference's (cemu-patches/0012,
`CEMU_TEX_DUMP_FRAME`, `CEMU_TEX_WATCH`) and ours (`WWHD_RENDER_DUMP`), compared surface by surface
in the reference's write order (`tools/reference/compare_dumps.py`), and a per-draw trace with a
pixel probe (`WWHD_RENDER_TRACE`). Beyond the draws themselves:

* **Capture timing.** The reference's shot N is the image its (N+1)th swap presents. The apparent
  "bloom halo" difference on the title was that off-by-one on moving frames; the renderer now
  captures the same way.
* **Transient targets share memory.** The game puts targets of other formats at one address within
  a frame, so a texture samples the surface *last written* at its address.
* **Padded targets.** Targets are allocated padded (1920x1088 for 1920x1080), so a texture of
  another size samples a copy of the surface's top-left corner, as Cemu's texture cache copies
  between overlapping textures.
* **Mip chains drawn level by level.** The bloom chain is one R11G11B10 texture whose levels are
  drawn as separate targets, so a surface-backed texture with mips is assembled from the surfaces
  at each level's address.
* **Array targets.** The shadow map is a D16 2D array drawn slice by slice (`DB_DEPTH_VIEW`) and
  cleared per slice; surfaces have array layers, draws attach the selected slice, and textures
  sample the slices their registers select. Before this, all slices collapsed into one and the
  title's sea showed a shadow the reference doesn't.
* **Overwritten surfaces read zero.** The G-buffer's normal target is never cleared; a 1920x1080 R8
  target inside its memory overwrites part of it every frame. The reference's texture cache then
  deletes the stale texture and reloads it from guest memory (zeros), at a swap, once a
  round-robin scan finds it unused for 100 ms of wall time, so after a host-dependent few frames.
  The renderer resets such surfaces at every swap. The remaining f510-f600 difference is the
  reference's few frames of old normals on edges before its reset.
* **Depth clears clear colour too.** The reference's depth clear also clears the colour textures
  that start at the same address; the renderer does the same.
* **`GX2CopySurface`'s CPU copies** (all 50 at boot involve a linear-special surface) are done in
  guest memory, as the reference's `LatteSurfaceCopy_CopyInRAM` does; the null GPU used to skip
  them. Without rendering the whole-route trace still equals `null-route.zst` (1,124,796,468 calls,
  native, 380 s).

Seen once and not reproduced: a segfault inside lavapipe's JIT code early in a run.

**M4 status (2026-09-29):** done on the scripted route. `WWHD_NATIVE=on` runs the recompiled
program: the hook calls the generated function at every entry, generated code ticks and yields
per instruction (D6 as built), imports go straight to Cemu's handlers (D4), and Cemu's boot
patches are in the generated code (D10).

* Over the whole route the OS-call trace equals the reference's: 1,124,796,468 calls to f10800.
  9,697,829 timeslices ended inside native code, each on the reference's instruction.
* **0 game instructions interpreted.** The only interpreted code is in Cemu's trampoline area. That's
  3.0M calls through a game function pointer that lands on the `blr` of Cemu's stub for an
  unsupported import, which is a no-op.
* The run takes 359 s, against 720 s interpreting and about 25 minutes for the reference on the
  GPU. Much of it is the trace writer (1.1 billion records through zstd) and the null GPU.
* Diff mode now also checks cycles. Over the whole route, all 3,875,261 sampled pure calls agree
  with the interpreter on registers, stores and the cycles charged (4,616 functions).

Performance work (D6's per-block counting, host-local registers) comes after the graphics track
needs it; every step keeps the same trace check.

**M3 status (2026-09-29):** done. Every sampled call of a pure function along the scripted route runs
natively and agrees with Cemu's interpreter, and the run's OS-call trace is still the reference's.

* **The seam** is Cemu patch 0011: `g_ppcExecuteHook` in place of the interpreter loop in
  `__OSFiberThreadEntry` and `PPCCore_executeCallbackInternal`, null by default. The reference with
  the hook null (600 frames) and `wwhd-null` with the hook installed but interpreting (the whole
  route, `determinism.sh`) both give traces identical to the baselines: 59,531,239 and
  1,124,796,468 calls.
* **The runtime** (`src/runtime`, linked into `wwhd-null` with the generated program) checks the
  code against guest memory at boot (two functions patched by Cemu, D10) and binds the 408 imports
  from it (D4). It implements all `rt_*`; the ones only non-pure code reaches (`rt_import`,
  `rt_call_ctr`, `rt_jump_ctr`) are bound and checked but first run in M4.
* **Diff mode** (D8.2 as built) over the whole route, checking each pure function's first 16 calls
  and every 256th after that: 978,805,396 pure calls, 3,875,261 checked, **all equal** (2,696 of them
  preempted mid-call), 0 escaped, 0 runaway, 0 native faults. 4,616 of the 12,968 pure functions
  run on the route, and all 4,616 were checked and are clean (2,984 of them at least 16 times). The
  trace equals `null-route.zst` (1,124,796,468 calls). The run takes 16 minutes, against 12
  interpreting.
* **Found on the way:** every jump-table `switch` was wrong since M2 (all 294 tables are `b` runs,
  see D1); `_iob+0x10` was emitted as `environ` (relocations are now keyed by symbol); the GHS
  restore-and-exit helpers need the check to end where the native run left the guest, not at the
  entry's LR; and after a context switch `spr.XER` holds stale copies of CA/SO/OV.

Diff mode leaves guest time to the interpreter; how native code accounts for it was decided before
M4 (D6).

**M0b status (2026-09-29):** done. `src/` builds two executables against the worker's Cemu
build (`src/build.sh`). Both use our frontend (`src/frontend`, Cemu's `WindowSystem` without
wxWidgets):

* **`wwhd`** keeps Latte on Vulkan. Its frames are byte-identical to Cemu_release's.
* **`wwhd-null`** is headless: `libCemuCafe.a` without Latte's 67 objects, with
  `src/gpu/null_gpu.cpp` in their place.

Along the scripted route, both give OS-call traces identical to Cemu_release's (59,531,239
calls to f600), and `wwhd-null` is deterministic run to run. Over the whole route into gameplay
(10,800 frames), `wwhd-null` matches the reference's 1,124,796,468 calls exactly, in 12 minutes
of wall time (the reference takes about 25 on the GPU). It is the fast harness for the CPU track. One trap: dropping objects changes
the order of static constructors, which moves Cemu's `SysAllocator` slots in guest memory. So
`wwhd-null` is linked with its archive members in `wwhd`'s order (`src/link_order.py`).

The reference also renders on the worker's Intel GPU now. Its full-route trace equals the
llvmpipe one (1,124,796,468 calls).

The CPU track needs no rendering, and the graphics track can use Cemu's interpreter for the CPU
(M0b), so the two proceed in parallel.

## Open questions

1. **Cemu build footprint.** Can Cemu's OS libraries (CemuCafe minus Latte, gx2 and TCL), Config,
   Input, Audio and Util build without wx and without vcpkg's full dependency tree? M0b answers
   this, and the coupling shim (D12) is the known part.
2. **`GamePatch` and graphic packs.** Which Cemu game patches target WWHD US (D10)? *Answered for
   `GamePatch` (M3): two, the race-condition `nop`s in `f_027F9994` and the DSP kill-channel
   branch in `f_028137E0`.* Graphic packs remain open.
3. **Fiber stack size and `musttail`.** Is 2 MB enough for native recursion? Check during M3/M4.
4. **Floating-point exactness.** Answered for the oracle (M1): Cemu does not fuse `fmadd*`
   (it is built for baseline x86-64, and we compile with `-ffp-contract=off`), rounds single ops
   through `float`, and truncates frC to 25 bits for single multiplies; the emitter copies all of
   it and matches bit for bit. The one tolerated difference is *which* NaN comes out when inputs
   are NaN, because x86 propagates the first operand and compilers may commute additions. Espresso
   follows different NaN rules again, so matching real hardware there is a later question.
5. **Shader state variants.** How many (program × fetch-layout × render-target-format) variants
   does WWHD really use? G0 answers this, and it decides specialisation constants versus
   enumerated variants. *Answered for the route (G0): 287 variants over 281 pipelines, so
   enumerate them.*
6. **Geometry shaders, MSAA, HiZ, stream-out.** Which of them does WWHD use (G0)? *None on the
   route; HiZ doesn't reach the command stream at all (D15).*
7. **Project licence.** Pick one before anything is published.
8. **GPU machine.** Which machine does M5 run on: your desktop, or a VM with GPU passthrough?
