# Recompiler design

Status: draft, 2026-09-28. Inputs: [`getting-started.md`](getting-started.md) (Phase 1 function boundaries),
[`research/2026-09-27-nwiiurecomp-eval.md`](research/2026-09-27-nwiiurecomp-eval.md), and source reading of
XenonRecomp, N64Recomp and Cemu (`main` @ `c717fcab`, 2026-09-24). Line and file references are to
those trees.

## Goal and non-goals

**Goal.** Translate every function of US v0 `cking.rpx` ahead of time into C++, and run it natively
on top of Cemu's Cafe OS and GPU emulation, packaged as a standalone program. It must:

* be correct first, with every step checkable against Cemu's interpreter;
* allow any single function to be replaced with hand-written C++, which is how enhancements
  and decompiled code land later;
* never ship game code: the translation runs on the user's machine from their own dump.

**Non-goals for v1.**

* Speed beyond "comfortably full speed".
* Other titles or versions.
* A GUI.
* Reimplementing Cafe OS or the GPU. We reuse Cemu.

## Architecture

```
 build time (user's machine)                        run time
 ─────────────────────────────                      ──────────────────────────────────────────
 cking.rpx ─┐                                       wwhd (native executable)
 functions.csv ─┤                                   ├─ generated/  ← recompiled guest functions
 jump_tables.csv ├─► recompiler ─► generated/*.cpp  ├─ runtime/    ← dispatch, HLE calls, helpers,
 symbols.csv ───┘   (tools/recomp)                  │               yield, diff mode, overrides
                                                    ├─ Cemu Cafe   ← RPL loader, coreinit scheduler,
                                                    │  (MPL-2.0)     HLE libs, IOSU/FS, Latte + Vulkan
                                                    └─ frontend    ← SDL3 WindowSystem, no wxWidgets
```

**The seam.** We keep Cemu's boot path as it is:

1. `RPLLoader` loads and relocates the RPX (text at `0x02000000`, data from `0x10000000`) and maps
   imports to HLE stubs in the trampoline area at `0x00E00000`.
2. coreinit starts its scheduler.
3. Latte runs its GPU thread.

We replace only guest *instruction execution*, in two places:

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
(`rpl_mapHLEImport`, `Cafe/OS/RPL/rpl.cpp:746`). WWHD has 426 imports.

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

1. **Instruction fuzzing.** For each of the 200 mnemonics, take random CPU states and memory
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
3. **End-to-end.** Boot to the title screen and a scripted gameplay path with `NWIIU_AUTOSHOT`-style
   frame dumps. Compare against Cemu running the same title with its interpreter or JIT, and check
   the frames by eye as well as by count.

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
  series for the D-seam: two call sites, an export of the HLE table lookup, and a frontend that
  implements `WindowSystem`.
  * Cemu's `CemuCafe` target already has no wxWidgets includes. It needs Config, Input, Audio
    and Util at link time (`src/CMakeLists.txt:209`).
  * The frontend is SDL3, replacing `gui/wxgui` and `main.cpp`.
* **Headless on this server:** Vulkan through Mesa's lavapipe (CPU software rendering) is enough
  for boot tests and frame dumps without a GPU. Real play needs a GPU machine.
* **Licences:** Cemu files stay MPL-2.0 (file-level copyleft), which is compatible with any
  licence for our own code. **We copy no nWiiURecomp code**: its licence is unclear.

## Milestones

| # | Milestone | Done when |
|---|---|---|
| M0 | Cemu as a library, headless | Our SDL3/headless frontend links CemuCafe and boots WWHD with the **interpreter** to the title screen under lavapipe, with frame dumps. This proves the runtime and gives the oracle before any recompiled code exists. |
| M1 | Instruction semantics | The generator emits all 200 mnemonics. The instruction fuzzer passes against Cemu's interpreter. |
| M2 | Whole program compiles | All 39,705 functions generate with zero unknown instructions and zero unresolved branches, and compile with clang. |
| M3 | Pure functions native | `native_dispatch` is on for pure functions only, with sampled diff mode clean over a boot-to-title run. |
| M4 | Everything native | HLE calls direct, yield points in place, `GamePatch` handled, boot to title natively with the interpreter fallback counter at 0. |
| M5 | Playable | A scripted gameplay path matches Cemu end to end, full speed on a GPU machine. |
| M6 | First enhancement | 60 fps interpolation as overrides (needs Phase 2 names). |

## Open questions

1. **Cemu build footprint.** Can CemuCafe, Config, Input, Audio and Util build without wx and
   without vcpkg's full dependency tree? This is the first thing M0 will answer.
2. **`GamePatch` and graphic packs.** Which Cemu game patches target WWHD US (D10)?
3. **Fiber stack size and `musttail`.** Is 2 MB enough for native recursion? Check during M3/M4.
4. **Floating-point exactness.** Does the host FPU plus correct rounding match Espresso for
   `fmadds` (fused vs. separate rounding) and denormals? The M1 fuzzer will show it; Cemu's
   interpreter documents its own choices.
5. **Project licence.** Pick one before anything is published.
6. **GPU machine.** Which machine does M5 run on: your desktop, or a VM with GPU passthrough?
