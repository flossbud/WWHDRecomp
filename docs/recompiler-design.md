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

**Looked at (2026-09-30), not worth it.** With `ctx` marked `__restrict` the compiler already keeps
registers in host registers within straight-line code and writes them back only before a call,
exit or yield, which is what host locals would do. Exactness forbids more: a callee (the GHS save
and restore helpers among them) may read any register, so every register must be current in `ctx`
at every call and exit, and a liveness analysis would remove nothing. Forcing the hot helpers
inline (single/double conversion, compares, CR updates) gained about 1% for 25% more compile time
and was reverted. Guest code costs about 3 host cycles per guest instruction; what is left is the
game's own work.

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
> **Per basic block (2026-09-30).** The planned optimisation, held to the same trace: a block (from a
label, or the instruction after a branch, call, import or runtime hook, to the next such end) of at
least 3 instructions checks once whether it fits in the slice; if so it is charged at once
(`RT_FITS`/`RT_CHARGE`) and runs without ticks, otherwise its checked copy runs with an `RT_TICK`
before every instruction, so the yield lands on the same instruction. Nothing inside a block can see
the difference: blocks end before anything that could read the budget, and guest time only advances
when a slice ends. Both route traces, command streams and sound stay identical, and diff mode on the
save route checks 1,118,003 calls of 4,345 functions with 0 mismatches (cycles included). The save
route runs in 31.8 s from launch, 1.89x real time (37.5 s before), the whole route in 131 s, 2.74x
(155 s); the CPU thread does 17% less work, guest code 22% less. The price: twice the generated
code (340 MB of objects) and about 10 minutes to compile.

**Then** (same day, same checks, 0 diff-mode mismatches): indirect calls go straight to the
recompiled function through a per-word table of host functions instead of the runtime's Dispatch;
`fcmpu` and the paired-single compares call an inline copy of Cemu's helper (fuzzed: 3.6 million
runs, 0 differences); the scheduler lock is an inline futex-style lock instead of a recursive
pthread mutex (`src/os/coreinit/coreinit_Scheduler.cpp`); `ctx` is `__restrict` (no measurable
change: the compiler already kept registers in host registers within straight-line code). Save
route 30.4 s, 1.97x real time; whole route 121.7 s, 2.96x; the CPU thread 8% lighter on the whole
route. Guest code is now 78% of the CPU thread, message queues 9.3%, `swapcontext` 2.9%: what is
left is the game's own work, where the next lever is keeping registers in host locals with a
liveness analysis (store only what a call or exit needs), and D19's context switch and queue
fast path.

The runtime keeps a callback depth per guest thread. Import calls and calls into the trampoline
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
* **Enhancements are overrides.** The plan for 60 fps was to interpolate camera and actor
  transforms (as setsail does); on 2026-10-01 the owner chose native 60 fps instead, and then an
  uncapped frame rate (D21).
* **Mid-function hooks** (XenonRecomp's `[[midasm_hook]]`) are deferred until a real need appears.

**As built (2026-09-30).** Not with weak symbols: a weak definition can't be inlined within its
shard, and every function would pay for the few that are replaced. Instead `config/US_v0/overrides.txt`
lists the replaced functions, and `tools/recomp/generate.py` emits each listed body as `orig_f_X`
(declared in `funcs.h` with the rest) and leaves `f_X` to `src/overrides/`. Every reference in the
generated code names `f_X`: direct calls, `musttail` tail calls, falling through into the next
function, and the function table (D5), from which the runtime's indirect-call table (`rt_direct`)
and its Dispatch are built, so all of them reach the override. The linker enforces the list: a
listed function without an override is an undefined `f_X`, an override of an unlisted one a
duplicate. The build (`src/CMakeLists.txt`) compiles `src/overrides/*.cpp` like generated code (`funcs.h`, `ppc_ops.h`,
`-ffp-contract=off -fno-strict-aliasing`) and links them with the recompiled program; only the
listed functions' shards change. An override reads like generated code (`GPR(n)`, `rd32`/`wr32`,
`RT_CALL_CTR`) and includes `src/overrides/override.h`. Checked with the first one (the task loop,
D19) in place: both route traces, command streams and sound identical, diff mode clean. The second
(2026-10-01, `src/overrides/gamepad_view.cpp`) skips the GamePad's screen in real time: the render
jobs draw each scene's views through `f_027D6BB0` (`gfx_RenderSceneView`), and a view whose render
target is the GamePad's 854x480 rectangle is the ITEMS menu for a GamePad that isn't there (9% of
the draws, about 1% of CPU). Forced with the virtual clock (`WWHD_SKIP_GAMEPAD=1`), exactly those
draws go and the TV's frames stay byte-identical; unforced, every check runs the game's code.
The 60 fps work (D21, WW-4) adds eight, in `src/overrides/sixty.cpp`: sead's `fw_procFrame`
(`f_0274C264`: whole or half tick, the random stream, the rollback, the store census), m_Do_main's
frame body (`f_025F172C`: the state probe after whole ticks), `fpcM_Execute` (`f_025DE58C`) and
`fopAc_Execute` (`f_025D475C`), which note the processes and actors that executed, `fpcM_Draw`
(`f_025DE2CC`: which process is drawing, for the census, and a probe that skips draws), and three
nodes of sead's tree held to whole ticks (`f_0260C74C`, `f_027618B8`, `f_0273CBD0`); the WW-3
prototype's override of the tick (`f_02746790`) is gone. `fpcM_Execute` also decides, per process,
what runs on a half tick (only converted processes) and with which time step. The conversion (D21,
2026-10-02) adds twenty-six in `src/overrides/sixty_step.cpp`: the game's per-tick helpers with a time
step for converted processes (the c_lib approaches and chases `f_0200ECD4` to `f_0200F8D0`,
`fopAcM_calcSpeed` `f_025D67A8`, `fopAcM_posMove` `f_025D6800`, `J3DFrameCtrl::update`
`f_027F2FC4` and `checkPass` `f_027F2BF8`, `decOldFrameMorfCounter` `f_025E3EC8`), countdowns that
count whole ticks (`cLib_calcTimer<u8>` `f_0207A9A0`, `dCcD_GStts::Move` `f_0251621C`), and
registrations with whole-tick systems a half step must not repeat (`dCcS::Set` `f_0200E240`, a
request list `f_02516C14`), and two moves measured over a whole tick (Link's `posMoveFromFootPos`
`f_023FCB9C`, the camera's `updateMonitor` `f_024F9A48`); each is the game's function unchanged
while the step is 1. With 60 fps and the probe
off each calls its original. Frame pacing (D21, the owner's feel test) adds sead's `fw_waitForVsync`
(`f_0274C874`, `src/overrides/pacing.cpp`): in real time at 60 fps a tick's two frames share their two
vsyncs, and `WWHD_FRAME_LOG` times every frame; otherwise it calls its original. Particles add two:
`dPa_control_c::calc3D` (`f_025A81A0`, sixty.cpp: whole ticks only at 60, or every frame converted;
it replaces a tick rule) and `JPABaseEmitter::calcCreatePtcls` (`f_0281F878`, sixty_step.cpp: the
wake's and bow waves' strips emit every frame). Adding or removing an override changes `funcs.h` (the `orig_f_X` declarations), which
every shard includes: the next build compiles all generated code again (about 12 minutes on the
worker), so batch them.

**Tick rules** (D21, 2026-10-02) are the per-instruction counterpart: `config/US_v0/tick_rules.txt`
lists calls and stores, by address, that run only on the game's whole ticks at 60 fps; the generator
wraps each in `if (RT_WHOLE_TICK())` (`g_rtHalfTick`, set by the runtime per frame, never at 30 fps)
and checks that the instruction at the address is the one the rule names; `late` runs it once a
tick at the tick's end instead (`RT_LATE_TICK()`: the half tick while its process steps at 60, the
whole tick otherwise: a tick counter both frames of a tick should see unchanged). They change only the
shards that contain them, so they rebuild in seconds, and they are how the generator's view of
every call site reaches the 60 fps work. *Step rules* (the conversion) are the same for converted code:
`keep:SRC` (a counter that counts whole ticks), and `OP:REG` / `OP@REG` (after the instruction, or
for that instruction only) with OP `*h`, `/h`, `k` (an approach's factor, `1 - (1 - k)^h`), `d`
(a damping factor, `d^h`) or `split` (an integer step split between the two half ticks); each is a
no-op while `g_rtStep` is 1 (`RT_STEPPED()`). `reload:fD=rB+O[+O2]` is for code that truncates a
frame count kept as a float (drawing too): at 60 (`g_rtSixty`) the float is read back. Every generated store also names its instruction to
the store journal (`rt_journal_store(ea, size, pc)`, only on the journaling path), so the 60 fps
tools report guest addresses.

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

Cemu graphic-pack code patches are handled the same way: they become overrides only when wanted.
(The pack that patches `0x025AC25C` is Cemu's "FPS Slowdown" workaround: a `nop` that keeps the
Miiverse thread `OliveOperationMgrThread` suspended. It doesn't touch the frame rate, and no pack has
ever changed WWHD's frame rate or speed; D21.)

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
  Its patches (tools/reference/README.md) make it deterministic and observable, plus two rendering
  fixes where it disagreed with the console: 0013 (a mip chain allocated below its base level, which
  the console's shared memory relates to its levels) and 0014 (a draw that samples its own colour
  target reads the image from before the draw: Cemu read it in place, which Vulkan leaves undefined
  and lavapipe answered in its shading order; the console's separate colour and texture caches read
  the pre-draw image). A fix goes in only with evidence that the console would draw it that way.
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
  the platform shell (own CMake build, done 2026-10-01; SDL3 window, input and audio device, which is
  also M5); the
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
  - ~~with the file system: `nn_save`~~: forked with the file system (2026-10-01, below);
  - with their whole library: `nn_act.Initialize` (loads accounts for the rest of nn_act),
    `nn_boss` and `nn_olv` (objects with vtables in Cemu's memory, IPC to Cemu's IOSU), `nlibcurl`
    and `nsysnet` (setup the rest of the library checks), and `OSGetSystemInfo` (returns a
    structure in Cemu's memory).
* **The file system (2026-10-01).** Forked whole: the FS client (`coreinit_FS.cpp`), the IPC
  driver (`coreinit_IPC.cpp`), the FSA service (`iosu_fsa.cpp`) and `nn_save`. The FSA service no
  longer runs on an IOSU host thread behind IOSU's kernel: the FS client serves each request in
  place, on the guest thread that made it, and delivers the reply to its core's IPC thread as
  IOSU's kernel did, with a message sent as a host thread sends one (it readies the IPC thread
  without switching to it). With the virtual clock, patch 0003 had already made IOSU deliver the
  reply before `IOS_IoctlAsync` returned, so the guest sees the same steps (the caller blocks on
  its command block's queue, the IPC thread runs the FS callback, the caller wakes); synchronous
  requests never blocked and are calls now. Along the routes the game calls 12 FS functions
  (about 1,000 reads a route, `FSGetVolumeState` once a frame) and 4 of nn_save's. All six routes
  stay identical, and the 41 save files the new-game route writes are byte-identical to what
  Cemu's FS writes. Still Cemu's: `fsc` underneath (portable: WUA archives through ZArchive, host
  folders), the console-shaped client structures, and `SAVEInit`'s account lookup through IOSU's
  legacy ioctls (patch 0006), which waits for `nn_act`.
* **Our own build (2026-10-01).** `CMakeLists.txt` builds `wwhd-null` with Cemu as a subproject
  from its patched source tree (patch 0015), configured as Cemu's own build is but without its
  wxWidgets GUI; at configure time CemuCafe loses Latte (all but what gx2 and the renderer use) and
  each fork takes its original's place in the Cemu target that had it. Our sources compile in
  CemuCafe's compile context, and the executable links like Cemu's own (at first twice, the
  second time with every archive member where the reference's link had it, so that Cemu's
  `SysAllocator` slots kept their guest addresses; the layout table below ended that). It replaced `src/build.sh`'s hand-edited
  copy of Cemu's link line, which needed a prebuilt Cemu tree; the result gives the same traces,
  command streams, sound and frames on all six routes. A first build takes 11-12 minutes on the
  worker, a one-file change about 10 s (ThinLTO's cache). For Windows, macOS or Android these files
  are where a port starts: per-compiler flags (ours are clang's), and our own code made portable
  first (the fibers' context switch, D19).
* **The guest OS objects' own home (2026-10-01).** Cemu's OS objects (threads, message queues, IPC
  buffers, the sound and GPU state the game sees) are 170 `SysAllocator` slots in the Cemu area
  (0x0E000000 up, 5.2 MB), laid out in the order their static constructors registered them: the
  link's. Ours take the reference's addresses from a table (`src/os/common/sysalloc_layout.h`, used
  by our fork of `SysAllocator.cpp`), by what each slot is: the file that declares it (with the line,
  for a header), its size and alignment, and how many alike that file declared before it (patch
  0016 has each slot record its declaration; lines aren't in the key for forks, whose lines move).
  Only three slots share a key, members of equal size in one header's class, so which takes which
  moves nothing. The bump allocator then carries on from the reference's end. With it, a plain link
  in any order gives guest memory as the reference's: all six routes and diff mode identical. That
  also holds wherever the build is linked, so a port's traces can be compared with the reference's.

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
  deterministic mode never uses them. (Built instead: the task loop sleeps through idle rounds,
  below.)

**Done so far** (2026-09-30): our own context switch (`src/runtime/fiber/FiberUnix.cpp`, a fork of
Cemu's: callee-saved registers, MXCSR and the x87 control word on the fiber's stack, no system
call; thread switches went from 3.6% of the CPU thread to 0.9%), and the queue operations skip a
division for in-range ring indices (exact). The switch-to-self shortcut needs D9's overrides and a
real-time mode to use it in, so it waits for step (2).

**Real time on one host thread** (step 2, 2026-09-30): without the virtual clock, guest time is
`steady_clock` (the forked `PPCTimer.cpp`; no 3-second TSC measurement at start); with no guest
thread runnable on any core the scheduler thread sleeps until one is queued (any host thread that
queues one wakes it), the next alarm, or 1 ms (sound and NFC polling); the null GPU sleeps until the
host-timed vsync when it waits for a flip or for commands instead of spinning; every 10 s the log
gets frame rate, frame-time median, 99th percentile and worst, and how busy the scheduler thread
was. The virtual clock takes none of these paths (save route: trace, command stream and sound
identical). Played on the owner's desktop (desktop CPU, AMD GPU on RADV, `tools/play/`),
headless along the save route: 30 fps from the title on, frame time median 33.3 ms and 99th 35 ms
once loaded; the hitches are first sights of shaders (worst 1 s during boot, 50-190 ms loading
Outset), which a pipeline cache on disk would take away on the next run.
**What real time showed:** the scheduler thread is busy 100% of the time and never sleeps: 85% of
it is the game's task switcher (`f_02760ACC` -> `f_0275FFCC`), which, with no task ready, keeps
switching to itself through its message queue (38% of the thread in `OSSendMessage` and
`OSReceiveMessage`). The Wii U's core spins the same way. The host thread only goes idle with an
override (D9) that recognises the empty round and waits for the next event (vsync, a message);
that is the real-time fast path above (built: *the task loop sleeps*, below).

**The task loop sleeps** (2026-09-30, the first fast path). The spinning thread (1603f4a8) runs the
game's task loop, `f_0275FFCC` (named `task_MessageLoop` in `symbols.csv`, with the rest of that
small task library): a task is an object with a message queue at task+32 on a guest thread of its
own; the loop receives messages until the task's stop message and passes each other one to the
task's handler (vtable slot 124). One task ticks: its handler posts itself message 5 after every
message, and a 5 does nothing while the object is idle, so the thread sends and receives its own
message forever (frame 1500 of the save route: 34,938 pairs and nothing else on that thread). The
override (`src/overrides/task_loop.cpp`, D9) is the game's loop in C++, plus, in real time only, a
watch on each handler call: the runtime's *quiet watch* (`src/runtime/dispatch.cpp`) turns on the
store journal that generated code already calls on every store (`RT_STORE`, diff mode's) and notes
any store outside the dead stack below the loop's frame (and its LR save word), interpreted code, OS
calls, and the thread leaving its core (its `wakeUpCount`). A call that stored nothing live, made one
OS call, and left the queue one message longer with its own message at the end, only posted its
message back; handling it again reads the same memory and does the same nothing. So when the loop
takes that message again and the queue holds nothing else, the thread first waits among the queue's
receivers (a sender wakes it, as `OSSendMessage` does) with a host alarm for 1 ms (the idle loop's
own interval), and when no guest thread can run the host thread sleeps. To the game that is the
thread being preempted: the same messages in the same order, the same writes, and anything only
memory or the clock tells the handler is seen at most 1 ms later. Productive rounds (the other task
threads decompress and mix audio) write memory and never wait. The fast paths run only with the
recompiled program in real time on one host thread (`wwhd::rt::FastPaths`: not with the virtual
clock, in diff mode or on three host threads; `WWHD_FAST_PATHS=0` turns them off).
*Measured* on the owner's desktop, the save route headless, same binary with and without: the
scheduler thread went from 100% busy to 8-25% (the rest is the game's work: audio mixing and
decompression under other tasks, and the frame), the process from 69.5 s to 23.2 s of CPU in 61.5 s,
30 fps and a 99th percentile of 34-35 ms either way; the loop sleeps about 800-930 times a second,
none of them ended by a message during play. With the virtual clock both routes' traces, command
streams and sound are identical, and diff mode is clean (the override is the game's loop there).

**Idle threads and clocks** (2026-10-01, real time, the worker, save route, per thread from
`/proc` over 30 s of play). Besides the scheduler thread (52% of a core) and the GPU thread (3%),
Cemu's input manager and audio backend ran four threads that do nothing for this program: its
input update thread (a 1 ms loop), the Wiimote and SDL controller providers' threads, and cubeb's
PulseAudio thread: 5.4% of a core and 2,870 of the process's 4,500 wakeups a second. The frontend
now stops them at start-up (the game's input calls are ours; snd_core opens no Cemu device unless
asked): 55% of a core and 1,640 wakeups a second. The GPU thread waited for commands or the next
vsync but woke at least every millisecond (967 wakeups a second); it now sleeps until a submission
or the vsync (137 a second, 1.8% of a core instead of 3.1%): 797 wakeups a second for the process,
82% fewer than before, with 30.1 fps and a 34.5 ms 99th percentile at full clocks. gx2, which D18 meant to rewrite next for
"submission and synchronisation", is 1.2% of the scheduler thread's samples in real time; guest
code is 83%. And clocks matter more than any of it: the worker's governor (`powersave`, 0.8 to
4.5 GHz) keeps a half-busy thread that sleeps a thousand times a second at low clocks, where the
save route runs at 27-28 fps with a 99th percentile of 64 ms; with a busy core beside it, 30.1 fps
at 34.7 ms and the scheduler thread 33% busy. Phones scale harder still, so real-time play there
will want the platform's performance hints (Android's ADPF) or fewer, longer sleeps.
The switch-to-self shortcut planned above is not needed: in play, the rounds that would use it now
sleep.

**Order**: (1) exact per-block cycle counting (D6's planned optimisation, held to the trace; done
2026-09-30: 1.89x real time on the save route, traces identical);
(2) real time on one host thread: host clock, sleeping idle, vsync from presentation (done
2026-09-30 but vsync, still host-timed); (3) our context switch (done); (4) heavier routes (sailing, a
dungeon, Windfall) timed, to decide on three host threads; (5) fast paths (the task loop's, done
2026-09-30, above; more where profiles show the game spinning).

**Heavier routes** (2026-10-01, item 4): sailing (`sail`), the menus and the Pictograph Box
(`menus`), and the Ballad of Gales' warp to the Tower of the Gods and down to Hyrule Castle (`warp`)
run at 2.05x, 2.54x and 2.22x real time with the virtual clock on the worker's power-capped i5 and
one host thread (the save route 2.24x): one host thread is enough so far.

**Open**: one host thread or three (item 4 decides; so far one); real vsync needs the GPU machine (open question
8); whether Cemu's three-thread mode has known behaviour differences for this game.

### D20. Shaders prepared before play, with a progress screen

*2026-09-30, after the first real-time play (D19): the hitches are first sights of shaders.*
The renderer (D13) translates a shader the first time a draw uses it: Cemu's decompiler (R600
microcode and registers to GLSL), glslang with its optimiser (GLSL to SPIR-V), then the driver
(`vkCreateGraphicsPipelines`), all on the GPU thread in the middle of a frame. Nothing was kept
between runs, so every start hitched again (up to 1 s at boot, 50-190 ms loading Outset, on the
owner's desktop). Modern PC games show a "preparing shaders" screen instead; this is ours.

**Three layers, cached separately** (`src/gpu/vk/shader_cache.cpp`, `portable/shaderCache/wwhd`):
1. *Shaders* (`shaders.bin`): per shader key (the renderer's, from the microcode's hash and the
   registers the decompiler reads), the SPIR-V plus what the draws read of the decompiler's
   analysis: uniform offsets, resource mapping, texture dimensions, sampler assignment, depth
   compare, colour outputs, the remapped-uniform lists, the quick buffer list. Starting from it
   needs no decompiler, no glslang, no game code and no registers.
2. *Pipeline recipes* (`pipelines.bin`): each pipeline's Vulkan state (vertex input, topology,
   raster, blend, depth/stencil, attachment formats) and its two shader keys; the recipe's bytes
   are its identity from run to run.
3. *The driver's cache* (`vulkan-<vendor>-<device>.bin`, a `VkPipelineCache`): used only if its
   header names this device and driver (`pipelineCacheUUID`); written at the end of preparing, every
   10 s while new pipelines appear, and when the window closes.
Layers 1 and 2 are the same on every machine and platform. They hold translations of the game's
shaders, so they stay on the player's machine, never in git. Records are appended as the game shows
something new, each with a checksum; a torn record at the end is cut off, and a header version
(`kVersion`) starts a file over when the translation changes.

**Before the game starts** (`PrepareShaders`, called by the frontend before the title launches):
every cached shader becomes a module, every recipe whose shaders are there is built on all cores
but one through the driver's cache, and once that has taken 0.3 s the window shows "Preparing
shaders" with a bar (the overlay on black; window events are handled, others wait). With a warm
driver cache it takes a moment and shows nothing. During play, anything new is translated as
before and added; the real-time log line counts these first sights and their cost.

**Checked**: `tools/reference/shader_cache_check.sh` runs the save route twice with the virtual
clock on lavapipe, from an empty cache and from the cache the first run filled: the TV captures
must be the same bytes, and the second run must add nothing (no first sights). It passes: 30
captures to f900 identical, 631 shaders and 430 pipelines prepared in 0.4 s on 11 threads.
Getting there found an old renderer bug: `Mix(a, b)` of two values alone is `(a ^ b) * P + C`, and
the pipeline-layout map and the per-run pipeline key started from two Vulkan handles, so pairs of
handles with the same XOR (common when they are allocated side by side, as preparing does) got
another pair's layout; lavapipe crashed on a binding the layout didn't have. Layouts are now keyed
by the pair itself and the pipeline key starts from `Mix(Mix(0, vs), ps)`.

**Measured** on the owner's desktop (AMD GPU, RADV), the save route in real time, headless:
* first start (empty caches, Mesa's too): 633 shaders (1.0 s of translation) and 432 pipelines
  (0.8 s) as first sights during the first minute, the worst frames 1 s and 347 ms;
* second start: prepared in 0.0 s, no first sights;
* as after a driver update (our cache, no driver cache, no Mesa cache): prepared in 0.1 s on 23
  threads, so on this machine the screen never shows. RADV compiles fast; it is for slower drivers.
The hitches left on a warm start (1 s during boot, ~100 ms loading Outset) are the same in every
run and have no shader work in them: the game's own loading.

**For Android** (and other platforms): the formats are plain bytes and SPIR-V; the directory comes
from the platform (`SDL_GetPrefPath` there); drivers there keep no disk cache of their own and
compile slowly (Adreno, Mali), so layer 3 is what makes the second start fast, and its header check
matters (some drivers crash on another device's data); the thread pool follows the core count;
records and the driver cache survive the app being killed. Layers 1 and 2 could be produced on the
PC that builds the port, so a phone never runs the decompiler or glslang for known shaders.

**Next**: (a) *the first start*: done (2026-10-01, "The shader list" below); (b) fewer pipelines
through dynamic state: measured and dropped (2026-10-01, below); (c) optionally, building a new pipeline off the GPU thread and skipping its draw until it is ready
(Cemu's asynchronous compile), a choice between a hitch and a missing object for a few frames.

**The shader list: a first start without hitches** (2026-10-01). `config/US_v0/shader_list.txt`
(`src/gpu/vk/shader_list.cpp` has the format) lists what playthroughs have met: for each shader its
key, the hash and size of its program and the content file it is in, its fetch shader, and the
registers translation read (nonzero values in the ranges Cemu's decompiler, its fetch-shader parser
and the PS input table read: about 1.5 KB of text a shader); and every pipeline recipe. That is
hashes, file names, register values and vertex layouts (a fetch shader is what GX2 builds at runtime
from the layout the game describes, not a file), no game content, so it is committed and shipped.
Before the game starts, everything in it that the cache doesn't have yet is made the way the
capturing run made it: fetch shaders from their code and registers, each shader's program read from
the player's own game files (Yaz0, SARC, SHARCFB, GFD, and shader files inside BFRES, as G1's
extractor walks them; the list names the content file) and translated with its registers alone in
the register file (it must give its listed key again), then its pipelines with the rest, under the
same "Preparing shaders" screen. *Capturing*: `WWHD_SHADER_SOURCES=path` makes a run record
everything it meets for the first time (it prepares nothing, so that is everything), and checks every
shader before recording it: translated a second time from the listed registers alone, it must give
the same key and the same record, byte for byte, or it is logged and left out (none was in the
save route's check, which recorded all 631 it translated). `tools/shaders/shader_list.py`
merges captures and finds each program's content file in G1's index. *Today's list*: the save route
on lavapipe, the title route and the tour route on the desktop, then the sail, menus and warp routes
and the new-game route on lavapipe (2026-10-01; the new-game route added nothing): 1382 shaders,
1028 programs in 49 content files, 76 fetch shaders, 1000 pipelines (3.1 MB of text, 210 KB
compressed). The first list was 866 shaders and 642 pipelines. *Checked*:
`tools/reference/shader_list_check.sh` (save route, lavapipe, virtual clock, from an empty cache: a
capturing run, then a first start from the list it made) gives 30 of 30 captures identical, and the
first start's cache holds exactly the capturing run's records (631 shaders, 430 pipelines), all made
before the game started (2.7 s) and none during play; the captures are also identical to those made
before the change. *On the owner's desktop* (AMD GPU, RADV), empty caches (Mesa's off too), in
real time: 866 shaders translated from the game files in 2.2 s and everything prepared in 2.3 s,
then the whole title route met 1 shader and 1 pipeline (5 ms) in 6 minutes and the save route
nothing, where a first start used to meet 633 shaders and 432 pipelines in its first minute (frames
of 1 s and 347 ms). Real-time runs aren't the captured runs exactly, so a rare state the captures
never saw is still translated when met; more captured playthroughs close that. With today's list,
first starts of the warp route and the new-game route (lavapipe, virtual clock, empty caches) prepare
1382 shaders and 1000 pipelines in 6.4-6.8 s and add no record during play. *For Android*: the
extractor reads only the 49 listed files (258 MB) through the game's file system, and the list holds
no machine-specific data.

**Dynamic state doesn't pay** (measured 2026-10-01, `tools/shaders/recipes.py` on the recipes of
both routes played in real time, 629 pipelines): nearly every pipeline is its own pair of shaders.
Making stencil reference and masks dynamic (core 1.0) saves none, extended dynamic state 1 and 2
(core 1.3) 5, and extended dynamic state 3 (blend, write masks, logic op, depth clip) 24 more, while
586 distinct shader pairs are the floor. What makes pipelines is shader variants (programs, and the
registers the shader keys hold), so the owner chose the shipped list of shaders and recipes (a)
instead. `PipelineDesc` is unchanged.

### D21. 60 fps (M6): what the game does each frame

*Started 2026-10-01 (WW-3); research done 2026-10-01 (WW-4): findings, a probe and the options
below.* **The owner chose option 1, mixed rate with verified conversion (2026-10-02).** **Decision (the owner, 2026-10-01): native
60 fps, then uncapped.** The game's own logic is to run at 60 ticks a second (and in the end at any
rate) and play exactly as at 30: the same speeds, jump arcs, timers, animations, cutscenes and sound
sync. Not interpolation: no extra frames drawn between 30 Hz ticks.

**The time base: there is none.** One tick is 1/30 s and every per-tick quantity is in units per
tick. Nothing in the game code takes a time step (the GameCube decomp has none; sead's framework
measures each frame's duration, `+0x78` and `+0x80` below, and nothing scales by it), so a slow
frame slows the game (Digital Foundry: "frame-rate is actually tied to its game speed"). Some HD
scenery (clouds, waves, sparkle) follows the clock instead and is already right at any frame rate.

**One frame, in order** (WW-3 mapped the framework, WW-4 the game inside it; names and evidence in
`symbols.csv` where named):
* sead's `GameFrameworkCafe` ("FrameworkCafe", created in main `f_02005EA8` with a vblank interval
  of 2; main also calls `GX2SetSwapInterval(2)` once) runs `fw_runLoop` (`f_0274C00C`), which calls
  `fw_procFrame` (`f_0274C264`, vtable 0x10004E88 slot 0xcc) forever: slot 0xd4, slot 0xdc
  `game_procFrameBody`, slot 0x6c, slot 0xec `game_procPresent` unless paused (`gfx_EndFrame`: the
  outputs' copies, flush, `GX2SwapScanBuffers`; ProcUI), the frame's duration (+0x78) and start
  (+0x80), slot 0xe4 `fw_waitForVsync`.
* `game_procFrameBody` (`f_02034FFC`): `f_02746790(root)` = sead's method tree calc
  (`f_02747C6C(root + 0x40)`, recursing through `f_02747BDC`), then `RenderDisplay_draw` and
  `RenderDisplay_calcGPU` (render job lists; the TV's view matrix is copied to `DAT_104b45f8`).
* The tree's game node is `f_0203593C`, the game task's calc: some HD work every frame (frame
  counters `f_020357CC`, the input manager `f_02617AF4(something_button_related)`, an environment
  update `f_0255E854`, a double-buffer flip `f_0278FEBC`, and a few more), and in the middle
  m_Do_main's frame body `f_025F172C`: a frame counter (`DAT_1048d0a8`, a heap check every n),
  `f_025E15E0`, then **`fapGm_Execute`** (`f_025D42EC`).
* `fapGm_Execute` = `fpcM_Management(NULL, fapGm_After)` (`f_025DF948`, "f_pc_manager.cpp") then
  `cCt_Counter` (`f_0200E6EC`: `g_Counter` at 0x101FF558, mCounter0 every call). `fpcM_Management`:
  MtxInit (`f_0200FAC4`), process deletion, priorities and creation, **the execute pass**
  (`f_025DE788(fpcM_Execute)`: every process's execute; actors through **`fopAc_Execute`**
  (`f_025D475C`, "f_op_actor.cpp": `old = current`, then the actor's own execute), **the draw
  pass** (`f_025DE37C(0x025DF908, 0x025DF904)`: every process's draw), then `fapGm_After`
  (`f_025D42C4`: the scene, overlap and camera managers) and an HD hook `f_02715310(some_gfx_ptr)`.
* CPU (the tour route, virtual clock, `WWHD_PROFILE_DEPTH=64` and `tools/profile_tree.py`):
  `game_procFrameBody` 38% of all samples; `fapGm_Execute` 24% (its execute pass 59%, draw pass
  37%); `RenderDisplay_draw` 30% of the frame body; actors are 63% of the execute pass, Link's
  execute (`f_0240CDD0`) 15% of that. The rest of the process runs on other cores: the job system's
  render jobs, sound, the task threads.

**Finding things in WWHD.** WWHD kept the GameCube code's assertions: a failed one calls
`f_0273AA24(file, line, expression)`. `tools/ghidra/source_files.py` lists every reference to a
source file name with the line argument: 2,429 references to 413 files, which ties WWHD functions
to the decomp's files (translation units are laid out alphabetically by file name, so a function
between two asserted ones usually belongs to one of their files). Actors: the 449 actor profiles
are the `.data` words pointing at `g_fopAc_Method` (0x101F3088; profile = word − 0x1C: process name
at +0x08, size at +0x10, the actor's own method table at +0x24). Process names are shifted from
the GameCube's: Link is 168 here (169 there; size 0x8284, execute `f_0240EBB0` → `f_0240CDD0`), the
boat 165. Actors keep the base layout: process name at +0x08, profile at +0x10, `old.pos` at +0x300,
`current.pos` at +0x314. The c_lib helpers sit together in WWHD: `cLib_addCalc` 0x0200ECD4,
`addCalc2` 0x0200ED84, `addCalc0` 0x0200EDC8, `addCalcPos` 0x0200EE00, `addCalcAngleS` 0x0200F378,
`addCalcAngleS2` 0x0200F428, `chaseS` 0x0200F564, `chaseF` 0x0200F5C8, `chaseAngleS` 0x0200F8D0
(caller lists from `tools/ghidra/decompile.py`: hundreds each). `cM_rnd` is `f_02019788`, its
Wichmann-Hill state at 0x101FF9D4.

**Where the per-tick steps are** (the GameCube decomp, counted by a study in WW-4; WWHD runs the
same game code):
* *Central, one place each:* the c_lib helpers (2,816 call sites, 2,602 in actors; exponential
  approach, linear chase, timers); `J3DFrameCtrl::update` (`mFrame += mRate`) for all skeletal and
  material animation, with crossing-safe `checkPass`; morph blends (`McaMorf::play`); actor
  integration `fopAcM_calcSpeed`/`posMove(F)` (semi-implicit Euler, 120 call sites); JPA particles
  (emitter and particle calc; their rates, lifetimes and forces are per-tick values in the
  resource files); time of day (`mCurTime += mTimeAdv`); cutscenes (JStudio `forward(1)`, integer
  frames, `mSecondPerFrame = 1/30`).
* *Open-coded in actors:* about 378 integration lines (75 actor files), 624 countdowns (167 files)
  plus 205 `cLib_calcTimer`, 151 per-tick random tests (`cM_rndF(1) < p`), 87 "every Nth tick"
  masks, 185 exact animation-frame tests (`(int)getFrame() == 15`, which fire twice or never at half
  steps). Link alone: 67 tuning tables (1,160 values) and about 60 countdowns. The camera
  (`d_camera.cpp`) hand-rolls 47 smoothing lines and 5 tick counters.
* *Per tick but not in execute:* the play scene's draw (`dScnPly_Draw`) runs collision resolution,
  moving collision geometry, all particle simulation, vegetation and `g_Counter.mTimer++`; the
  sea's and the sky's draws advance counters; sound runs frame-counted fades every frame. One
  global random stream (1,378 call sites) serves everything.
* *Exactness:* `v += a; p += v` is a parabola; two plain half steps make Link's full-speed jump
  5.6% higher. For any step h (in 30 Hz ticks), `v += h·a; p += h·v + (1 − h)/2·Δv` reproduces the
  30 Hz arc exactly at whole ticks; approach factors become `1 − (1 − k)^h`, damping `d^h`, caps
  and minimum steps `× h` (BotW's `ksys::VFR` helpers have these forms). Integer angle steps need
  their remainder carried; integer timers and cadences keep a 30 Hz phase.

**Prior art.** Every other Wind Waker port interpolates (setsail and ZeldaWWHDRecomp for WWHD,
DeepSea, Dusklight). The GameCube 60 fps codes change one tick rate, and the game runs at double
speed (our swap interval prototype does the same). Meowmaritus' hack (2016-17) scaled floats and
ran timers every other frame; it softlocked in places. The closest is the Wind Waker Recomp's
experimental "60 Hz gameplay" (a static recompilation of the GameCube game, 2026-09-29): 53
instruction sites with a half step and a "legacy" 30 Hz phase (movement and gravity, Link's
integration with the arc correction, animation, the helpers, timers, particles; the frame counter
and the sound pump on the 30 Hz phase only), switched off in cutscenes, events, menus and
transitions. Its jump apex matches within 0.2%, running starts and ladders still differ by about
10%, and with the mode off its 201 player-state records equal the original's. Engines that already
have a time step (Perfect Dark, BotW with FPS++, Unleashed) still needed per-site fixes.

**The probe (WW-4).** Built to answer whether the game's logic can be held to 30 ticks a second
inside a 60 Hz frame, and to start the measuring tool (`src/overrides/sixty.cpp`, `tools/sixty/`):
* `WWHD_60FPS=1` now works with the virtual clock too; `WWHD_60FPS_FROM=N` switches at swap N, so a
  route reaches play at 30 first (menus and loading take game time: switched from boot, the route's
  presses landed elsewhere and never reached play). From there a *whole tick* (even swaps) runs
  m_Do_main's frame body as the game's; a *half tick* runs fpcM_Management's draw pass only
  (`WWHD_60FPS_HALF=draw`, default) or nothing of it (`=none`). Input scripts count game frames
  (`wwhd::rt::GameFrame`).
* **It doesn't hang.** Both variants play the whole tour route at 60 frames a second (the WW-3
  prototype skipped the whole tick, render preparation included, and hung).
* **Equal game clocks.** With the virtual clock a guest instruction is a cycle and the three cores
  share one clock, so a 60 fps frame with game logic took more than one vsync and the run fell
  behind (93 s of game clock for the 30 run's 75). `WWHD_VIRTUAL_SPEED=3` (three instructions a
  cycle, both runs) makes every frame fit: game clocks at matching ticks then agree within 1 ms.
* **The state probe** (`WWHD_STATE_DUMP=dir`): after every whole tick a hash of every executed
  actor's bytes, every 30 ticks their bytes, a few globals, and at chosen ticks all of .data/.bss;
  `tools/sixty/run.sh ROUTE OUT` makes the 30 and 60 runs, `tools/sixty/compare.py` lines them up
  by game frame. **The store census** (`WWHD_STATE_CENSUS=1`): during half-tick frames the store
  journal (D8, D19) hands every store to a hook with the host address it came from, so
  `tools/sixty/census.py` lists which recompiled functions write actors and game globals between
  ticks (`WWHD_STATE_CENSUS_TRACE=addr` adds the guest call chain from the stack's back chain).
* **What it found, with nothing of the game's frame on half ticks:** 13 actor types still diverge,
  the boat from the first tick. Between ticks, outside the game's frame: `f_0255E854` (the
  environment update the game task calls every frame) draws one `cM_rndF` a frame and smooths
  globals with `cLib_addCalc`, `f_020357CC` advances three frame counters, and the sound threads'
  state moves with timing. The random stream is the main channel: every random decision after it
  differs. With the draw pass on half ticks, `g_Counter.mTimer`, more random draws and the draw
  pass's simulation (as the decomp says) come on top. Of 9 million stores in half-tick frames,
  nearly all go to render and sound scratch; one actor (process 68) is written by `f_025AA12C`.

**What this means.** No single switch exists: per-tick steps are everywhere, but the common ones go
through a few central functions, and the tick has a clean seam: the game's own frame
(`fapGm_Execute`) inside a per-frame shell (HD work, render jobs, present). Exact equality at equal
game times is possible only for what still runs at 30; converted motion matches within a tolerance
(the arc correction makes the common case exact at whole ticks), random decisions diverge once
anything draws at a different rate, and cutscenes need fractional JStudio frames.

**Options** (put to the owner 2026-10-01; **chosen 2026-10-02: 1**):
1. *Mixed rate, convert and verify one system at a time (recommended, chosen).* Every frame runs at 60; the
   game's frame is split at its seams (process execute, the draw pass's simulation, the HD
   per-frame work, sound, `g_Counter`), and everything not yet converted runs exactly as at 30 on
   whole ticks, so its speed is always right. A converted system steps every tick with a time step
   h: dt-aware overrides of the central functions above, plus small rules the generator applies at
   exact instructions for open-coded steps (the census and the probe find them), each checked
   against the 30-tick run (whole ticks within tolerance, half ticks between their neighbours).
   Order by what shows: camera, Link, animation, the boat, particles, HUD, then actors route by
   route. Uncapped is the same machinery with a variable h and a 30 Hz phase accumulator.
2. *Convert everything at once* (the Wind Waker Recomp's model): tick everything at 60, halve the
   steps at the central functions and in Link's code, keep integer timers and counters on a 30 Hz
   phase, and fix what shows up. Quick to a first result, but every step not found yet (hundreds of
   open-coded timers and moves) runs double speed until someone meets it, and nothing tells which
   systems are done.
3. *Rescale constants for a fixed 60* (no time step): halve speeds, quarter accelerations and
   double durations in code and data. No runtime plumbing, but it can't go uncapped, constants are
   shared between uses, and it is how the GameCube hacks got physics wrong.
Considered and ruled out: running half steps speculatively from the last state and rolling them
back through the store journal (exact at whole ticks for free, but it draws frames between 30 Hz
ticks, which the owner ruled out); interpolation (the WW-3 plan).

**The steps from here** (the owner checks each):
1. *The measuring tool, with an exact baseline.* With nothing converted, the 60-tick run equals the
   30-tick run at every whole tick: the leaks the census found (the HD environment update and its
   random draw, the HD frame counters, the play scene's draw simulation, `g_Counter`) run on whole
   ticks only. Then captures at equal game times and the frames between, sound starts by game
   time, and reports with actor names.
2. *Conversion, one system at a time*, each against the baseline (D21's formulas, dt-aware central
   functions, generator rules for open-coded steps): camera, Link, animation, the boat, particles,
   HUD, actors route by route; the owner judges the feel on the desktop, windowed.
3. *Uncapped:* a variable step with a 30 Hz phase accumulator, presentation without vsync, frame
   pacing.

**Step 1: the measuring tool and the baseline (WW-4; the owner chose to finish it on every
route before converting anything).** At 60 fps every frame is
either a *whole tick* (the game's 30 Hz logic, exactly as at 30) or a *half tick* between two; the
baseline is "nothing converted": the 60-tick run must equal the 30-tick run at every whole tick.
* **Tick rules** (`config/US_v0/tick_rules.txt`, applied by `tools/recomp/generate.py`): 39
  instructions, each a call or a store at an exact address, that run only on whole ticks
  (`if (RT_WHOLE_TICK())` around their generated code; always true at 30 fps, so every check is
  unchanged). Each names the instruction it expects, so a wrong address is a generator error. They
  hold to whole ticks: m_Do_main's frame counter and calls, `cCt_Counter`, `fpcM_Management`'s
  deletion, priorities, creation, execute pass and `fapGm_After` (its draw pass runs every frame);
  in the play scene's draw (`dScnPly_Draw`, `f_025AF8A0`, matched call by call with the GameCube's)
  collision resolution and `ClrMoveFlag`, the next-stage requests (through `dComIfG_resetToOpening`,
  r3=1), vibration, magma, `PrepareMass`, grass, trees, wood, flowers, poison light, moving
  collision, snap, particle calc 3D/2D/menu, the `g_Counter.mTimer` store, `MassClear` and
  `Ccsp()->Draw()` (which rewrites the mass manager in WWHD); and in the game task's calc the HD
  frame counters, the input manager, the HD UI's per-frame update (`f_02614F74` with the UI
  manager `DAT_101f4ff0`), `f_0270870C` and the environment update. `fw_procFrame`'s
  override decides per frame (`g_rtHalfTick`).
* **The random stream** (`cM_rnd`'s state at 0x101FF9D4) is saved before a half tick's frame and
  put back after: the lighting that every actor's draw calls (`f_025615B8`, through a
  `settingTevStruct`-like function) takes random numbers (flicker), so drawing at 60 moved the one
  stream all decisions use.
* **Draws leave no trace** (`WWHD_60FPS_ROLLBACK`, default 2): during a half tick's frame every
  store the main thread makes into a process that executed in the last whole tick (actors, the
  camera, the environment, the HUD and menus, scenes) or into the game's .data and .bss is
  journaled through the store hook and put back when the frame ends (by then `gfx_EndFrame`'s
  `GX2DrawDone` has let the GPU finish with it). The census found why: draws write their lighting
  blend state (`tevStr`), culling flags and display-list pointers into their process, some advance
  animations or counters there, and some run logic (the raft's light flicker, `f_02363374`, picks
  random targets from its draw; the HUD's draw moves counters in the game info and statics beside
  the menu flag; the environment's draw blends light transitions in `g_env_light`). Level 1 (only
  processes and `g_dComIfG_gameInfo`) left the sail route's light transition and the menu's statics;
  level 3 (also everything a process's draw writes to the heap) broke state that has to persist
  (diverged from tick 1297), so it stays a probe.
* **More rules from the routes:** the HD UI's per-frame update (`f_02614F74`) and the HD game
  manager's per-frame update (`f_02715310(some_gfx_ptr)`, beside the menu controller `f_027152B4`
  that sets and clears the menu flag: run every frame, it closed the pause menu a tick early); and
  three other nodes of sead's tree held to whole ticks by overrides (`f_0260C74C`, `f_027618B8`,
  `f_0273CBD0`, `WWHD_60FPS_NODES`; one allocated effect or sound handles every frame).
* **Where the baseline stands** (all routes from the 100% save, 60 fps from swap 900, the draw pass
  on half ticks; `tools/sixty/run.sh` and `compare.py`):
  - *save, tour, sail, menus:* every actor equals the 30-tick run at every whole tick: walking,
    camera swings, the pause menu and its pages, the item screen, the Pictograph Box, the sea chart,
    boarding the boat, sailing and turning on the open sea (through a light transition).
  - *warp:* equal through the Wind Waker, the Ballad of Gales, the cyclone, the Tower of the Gods and
    the descent into Hyrule Castle; near the end two actors differ in a few fields (the pirate
    flag's cloth packet from tick 3240, a byte of a Moblin from 3450) without spreading.
  - *Sound:* requests are serviced up to half a tick sooner (the sound engine runs per frame): the
    menu's voices start at game frames 1563.5 and 1982.5 instead of 1564 and 1983. Render globals
    and the sound threads' state move with timing.
  - With nothing of the game's frame on half ticks (`WWHD_60FPS_HALF=none`) every actor is equal on
    the tour route too, and whole ticks' pictures equal the 30-tick run's pixel for pixel.
* **Pictures.** With nothing of the game's frame on half ticks the whole ticks' captures equal the
  30-tick run's pixel for pixel, and a half tick repeats the frame before it exactly: the HD renderer
  keeps its draw lists and renders them again when the game's frame doesn't run. With the draw pass
  and the rollback (the sail route, GPU captures) a half tick's picture is whole (models, HUD,
  wake) and matches its neighbour but for the clock-driven sea foam; whole ticks are within
  58-84 dB of the 30-tick run's.
* **Sound.** At the OS level both runs make the same calls (14 voice starts, 956 volume changes on
  the tour route); WWHD mixes its effects inside its own sound engine, so per-effect timing needs a
  hook on the game's sound calls (`mDoAud_seStart` and friends), still to come.
* **Measuring needs** (`tools/sixty/run.sh`): the switch at a frame in play (`WWHD_60FPS_FROM`, menus
  and loading take game time) and `WWHD_VIRTUAL_SPEED=3` (with the virtual clock all three cores
  share one clock; at 60 fps a frame with game logic took more than one vsync until the emulated CPU
  was made faster; both runs use the same speed, and their game clocks then agree within 1 ms).
* **Tools:** `tools/sixty/run.sh ROUTE OUT` (both rates, the state probe, `SIXTY_TRACE` for an OS-call
  trace), `compare.py` (by game frame: actors' hashes, fields, globals; `--names`), `census.py` (what
  half ticks write, by recompiled function), `actor_names.py` (WWHD's process numbers to the
  decomp's names: 319 of 449, by assert files and order), `WWHD_STATE_CENSUS_CHAINS=1` and
  `WWHD_STATE_CENSUS_TRACE=addr` (guest call chains from the stack's back chain),
  `WWHD_STATE_DUMP_GLOBALS=ticks` (all of .data/.bss and every actor at those ticks),
  `WWHD_STATE_CENSUS=2` (whole ticks too).

**Step 2: conversion, the machinery (WW-4, 2026-10-02; the owner chose the camera and Link first).**
* **Converted processes** (`WWHD_60FPS_CONVERT=n,m,...`, process names: the camera 476, Link 168)
  run their execute every frame at 60 fps with `g_rtStep` 0.5 (the part of a 30 Hz tick a frame
  is); every other process runs on whole ticks with a step of 1. The execute pass now runs every
  frame (its tick rule is gone) and `fpcM_Execute`'s override returns at once for an unconverted
  process on a half tick; a converted process's stores stand (they are not journaled), and its draws
  are out of the half ticks' rollback, being part of its frame.
* **The helpers with a time step** (`src/overrides/sixty_step.cpp`, the game's own function while
  the step is 1): the c_lib approaches (`cLib_addCalc` and its six relatives: factor
  `1 - (1 - k)^h`, maximum and minimum steps `× h`), the angle approaches (integer divisor and steps
  converted and rounded: not exact), the chases (step `× h`), `fopAcM_calcSpeed` (gravity `× h`, and
  what it added is noted), `fopAcM_posMove` (`p += h·v + (1 − h)/2·Δv_g`, Δv_g what gravity added in
  that step, so a jump's impulse isn't spread and two half steps land on the 30 Hz arc; the
  collision's push-out `× h`) and `J3DFrameCtrl::update` (`mFrame += mRate·h`; a mode that stops or
  turns the animation keeps that). Still to come: `checkPass` (it looks one rate ahead, so it
  would fire on both half steps), morphs, `cLib_calcTimer`.
* **The step-doubling trial** (`WWHD_60FPS_TRIAL=n,m,...` with the probe, at 30 fps): at every
  tick each listed process's execute runs as two half steps (the second as a half tick, as at 60),
  every store the main thread makes journaled; what they leave is noted and put back, and the
  execute runs once as the game's, which the run goes on from. Each store whose bytes differ is
  counted by the instruction that made it (`trial.txt`, `tools/sixty/trial.py`): with the value
  before the tick, after the half steps and after the game's step, and what that suggests ("twice":
  an unconverted per-tick step; `x(2 − k)`: an unconverted approach). It finds per-tick code one
  tick at a time, without the run drifting (a 60 fps run diverges for good at the first one).
  `tools/sixty/rmw.py` lists a function's read-modify-write stores with the instructions that
  compute them (from `tools/ghidra/disasm.py`'s output), the candidates to read with the decomp.
* **Store addresses:** every generated store names its guest instruction to the store journal
  (`rt_journal_store(ea, size, pc)`, only on the journaling path; the census and the trial report
  guest addresses, `tools/sixty/functions.py` names their functions).
* **Step rules** (D9): `keep:SRC`, `OP:REG` and `OP@REG` with `*h`, `/h`, `k`, `d`, `split`, at exact
  instructions, checked by the generator like tick rules.
* **Tracking** (`WWHD_STATE_TRACK=n,m,...` with the probe): those processes' bytes after every whole
  and half tick (`track.bin`); `compare.py --track` reports converted ones within a tolerance:
  position error at whole ticks, the floats that differ most, and at half ticks the distance from
  the midpoint of the 30-tick run's two ticks around them.
* **First measurements** (tour route): with nothing converted the 60-tick run still equals the
  30-tick run at every tick. With the camera and Link converted and only the helpers, Link ends up
  4,233 units off: their per-tick code is mostly their own. The trial lists 1,534 differing
  stores. The camera's: its five tick counters (`m07C`, `m080`, `m108`, `m118`, `m11C`), the
  monitor's per-tick move (`updateMonitor`), and about 50 approaches in `followCamera` (factors
  constant or computed, scalar, vector through `cXyz` scaling, and angle through `cSAngle`
  multiplication), its turn and charge counters and the `m384/m380` ramp. Link's: the move speed
  he takes from his feet's animation (`posMoveFromFootPos`), his animation and its frame tests, his
  own gravity and integration, timers.

**Step 2: the camera and Link (WW-4, in progress).**
* **Events fall back to whole ticks.** A converted process steps at 60 only while no event runs
  (`dComIfGp_event_runCheck`, the byte at `g_dComIfG_gameInfo` +0x5292); during events and cutscenes
  it runs on whole ticks with a step of 1, as at 30 (the GameCube recomp's 60 Hz mode does the same).
  The mode is chosen at each whole tick for it and the half tick after, so a switch falls between
  ticks; a half tick also doesn't step if an event started or was ordered since (`dEvt_control_c`'s
  order count, two bytes before its mode: the event manager starts an order on the next whole tick,
  and Link's Wind Waker wait, ordered from his own tick, cancelled itself on the half tick between).
  Without the fallback the warp route's songs and cyclone ran Link's and the camera's event code
  twice a tick (Link ended 7,749 units off; with it he plays the song and warps).
* **Button presses once a tick.** The HD input object (`*0x101F5088`: +0x18 pad 0's presses, +0x124
  held, +0x130 the stick) is updated on whole ticks (a tick rule), so on a half tick last tick's
  presses read as new: Link's `setStickData` took them twice. With conversions on, +0x18 reads 0 on a
  half tick and is put back after it.
* **The camera** (about 110 rules): `updateMonitor` (the player's move per step read per tick, its
  0.075 smoothing, an idle counter), `Run`'s five tick counters, the bank's damping and the forward
  cushion, `followCamera` (its approaches: scalar, vector through `cXyz` scaling, angle through
  `cSAngle` multiplication, and a factor vector applied component-wise; its turn, charge and ramp
  counters; the `m384/m380` ramp's weight) and the HD port's own follow engine (`f_0250FDC8`, no
  GameCube counterpart: the same kinds, plus targets that the stick rotates each tick and a distance
  chased by a fixed step, through a clamp helper `f_024F7DF4`). The trial's camera list fell from
  about 200 differing stores to the coupling below. *Coupling:* `followCamera` moves the eye 0.75
  of the way to `center + direction` and then takes the direction from `eye - center`, so the eye
  approaches its target with factor `0.75 k` a tick; converting both factors separately gives
  `approach(0.75) approach(k)` a half step, and the eye turns up to a third slower for small `k`
  (the trial: 0.68-0.76 of the 30 Hz move; the HD engine 0.86). So the direction's factors are
  converted with the eye's: `approach(0.75 k) / approach(0.75)` (the step rule `k75`), the product
  `approach(0.75 k)` a half step (the trial: 0.81-0.90 after; the HD engine, without that
  structure, still 0.86).
* **Measured over a whole tick.** Two quantities are a distance moved since the last step, read as
  a speed: the camera's `updateMonitor` (the player's move) and Link's `posMoveFromFootPos` (his
  planted toe's move, from his animation). Divided by h they double whatever moves per step rather
  than per tick: the feet's step-to-step jitter (their ground fitting; Link's speed ran 20-30%
  high) and, worse, a move made on whole ticks only (on the boat the ship moves Link, 33 units at a
  whole tick and none at the half tick: the camera read 66 and 0). Their overrides keep the last
  two steps' positions and measure from the one a tick ago, with no division: the 30 Hz
  measurement exactly at whole ticks, a centred one between them.
* **Link:** `posMoveFromFootPos` (the planted toe's move per step read per tick, its 0.3/0.7
  smoothing, gravity `× h` in both branches, `current.pos += speed` with the arc correction through
  the new `note:` and `arc@` rules), `posMove` (a whirlpool's pull, ice, belts, wind `× h`; the
  collision push and the wind push are used once and cleared, so they stay), `setNormalSpeedF`'s
  acceleration, and shared helpers: `checkPass`, `cLib_calcTimer<u8>`, `dCcD_GStts::Move`'s
  countdown, `decOldFrameMorfCounter` (the old pose's blend: `h` a step, a whole 1 when it starts),
  and two registrations with whole-tick systems that a half step must not repeat (`dCcS::Set`, the
  colliders for the next resolution; a 5-entry request list). Integer helpers now split their steps
  exactly (`s - s/2` then `s/2`; an approach's divisor `2s - 1` then `2s`, since
  `(1 - 1/(2s-1))(1 - 1/(2s)) = 1 - 1/s`).
* **Real time:** the fast paths' quiet watch (D19) and the half ticks' store hook share the store
  journal: every store goes to both, and turning one off leaves the journal on for the other (on the
  desktop a task-loop watch inside a half tick would have read as quiet, and switched the half
  tick's rollback off when it ended). The virtual clock has no fast paths, so only real time (and
  the owner's test) sees this.
* **The boat** (`WWHD_60FPS_CONVERT=165`, about 35 rules; WWHD's `daShip_c` fields sit 0x2E8 on
  from the GameCube's): its execute (two countdowns, the pitch spring `m0386 += force; m0384 +=
  m0386` with integer angles split between the half ticks, the turn rate, a wake phase, its move
  per step read per tick, the path's current), `setWaveAngle` (the roll and pitch springs toward
  the waves), `setYPos` (riding the water: an approach, a bob spring, random phases and wobbles,
  each per-tick part `× h`; the random draws mean the wave motion matches in kind, not number for
  number), `setMoveAngle` (the heading turns by the tiller each tick) and `setControllAngle` (the
  tiller's spring). Its speed goes through `cLib_addCalc` (converted). Without the steering rules
  the boat turned twice as fast and ended 63,000 units off; with them it sails the 30-tick run's
  course within 84 units (one tick of sailing ahead).
* **A half tick's draws see the whole tick's shared state.** A converted process's half step
  writes the game's globals too (Link sets gameInfo's player status and the action the HUD shows
  every step); the draws of unconverted processes then met a state their own update (whole ticks
  only) hadn't prepared, which the 30 Hz game never reaches. In a real-time run on the desktop the
  HUD's draw crashed once in its text code just after the switch to 60 (the HUD's own half-tick
  draw turned out to be the cause, below; this is still right). What converted processes write to .data and .bss during a half
  tick's execute pass is now hidden from its draw pass (the whole tick's values put back before the
  first draw) and handed back after the frame's rollback; on the tour route that is 115 words
  (the matrix stack, J3D scratch, gameInfo's player status, the random state). The tracked
  measurements don't change, and consecutive captures still all differ (the scene moves every
  frame). `hidden.txt` (with the probe) lists the words.
* **The HUD isn't drawn on half ticks.** Booting at 60 (no switch at swap 900) crashed every time
  just after play began, converted or not: the HUD's draw (METER, process 481) rebuilds its text
  on a half tick when what it shows changed, and the rollback put back its pointers but not the
  heap they pointed into, so its next draw read freed text (no crash without the rollback, without
  the HUD's half-tick draw, or with nothing on half ticks; the one real-time crash after a switch
  at 900 was this too). The HD port renders the HUD's layouts every frame from their own state, so
  half ticks still show it (captures: the HUD whole on both kinds of tick). `WWHD_60FPS_SKIPDRAW`
  now defaults to 481. The unconverted baseline stays exact (tour, menus).
* **Real time** (the owner's desktop, headless, 2026-10-02): the tour route at 60 with the camera,
  Link and the boat converted runs at 60.1 fps, frame times median 16.7 ms, 99th percentile
  18.4 ms; without conversion 56.8-60 fps. Booting at 60 (the save route, converted): 59.6-60
  fps, median 17.7-18.8 ms, 99th 22-25 ms, no crash in two runs.
* **Discrete choices from requests stay on whole ticks.** The camera's type comes from requests
  other processes leave (`+0x528`) and the camera consumes; an unconverted requester leaves one
  on whole ticks only, so the camera took the requested type on a whole tick and dropped it on the
  half tick, restarting its style every frame and holding still (sail route: the Fishman, process
  118, asked for his camera, the boat being a tick of sailing nearer him than at 30). `nextType`
  now runs on whole ticks only (tick rule `whole:r3=r4`: on a half tick it returns the current
  type).
* **Where Link stands** (60 fps from swap 900, camera and Link converted): on the tour route his
  normal speed now ramps at the 30 Hz rate (`1.75` a half step, `3.5` a tick), half a tick early
  because his state changes can fall on a half tick (60 fps reacts sooner); his worst position error
  is 24 units (after the toe fix; 41 before), a lead of about two ticks of walking; the camera's
  view direction is within 1.2 degrees of the 30-tick run's on average (7 at worst). Sail: worst 118 (boarding), 23 on average;
  menus: 0; warp: he plays the song and warps, the last scene within 0.2 units, the one before off
  by a tick at its start (created a tick later) and then 30-70. The speed he takes from his feet still runs
  high in the first ticks of a walk and spiked once against a wall (with only Link converted he then
  slid past it, 219 units off). The trial's half steps disagree with real 60 fps runs for Link (they
  run back to back without the world between them; his foot checks read ground state): for him the
  60 fps runs with `tools/sixty/track.py` (fields tick by tick, half ticks too) are the measure.

**Step 3: the owner's feel test (2026-10-02).** The owner played the converted build in a window
on the desktop: smooth with the sea in view, a chug and slow motion facing Outset, the sail and
the boat's wake flickering, and a question whether the boat is too fast.
* **The boat's speed is right.** Its move per tick from the tracked runs, 30-tick and 60: sailing
  north 33.06 and 33.06 units, turning west 46.11 and 45.74, open sea 49.94 and 49.93, after the
  gentle turn 44.57 and 44.58.
* **Why it chugged: frames that missed their vsync.** sead's frame ends in `fw_waitForVsync`
  (`f_0274C874`): the next vsync, then every swap flipped. At 60 a tick is two frames, and the game
  is frame-locked: a frame that overruns 16.7 ms waits for the vsync after, its tick takes 50 ms
  instead of 33, and the game runs slower. The owner's log facing Outset: 41-48 fps, frame times
  of 17 and 33 ms. Two causes, both found with the frame log (`WWHD_FRAME_LOG`, `tools/sixty/frames.py`:
  per frame the work, the GX2DrawDone wait, both threads' CPU, vsyncs missed) and the profiler
  (`WWHD_PROFILE` in a headless real-time run):
  * *The half tick's journal* took a fifth of the main thread: every store of a half tick's frame
    went through `RollbackStore`'s range search (a binary search over the executed processes), and
    a store into journaled memory was saved each time, repeats included (the matrix stack, a draw's
    fields). Now which 4 KB pages hold journaled memory is worked out once a frame, and each keeps a
    bit per byte saved: a store is saved once. Sail route: 540-820 thousand stores a half tick,
    about a thousand saved. The rollback is the same (saves restored last to first).
  * *The renderer's thread*, which the game waits for at the end of every frame (gfx_EndFrame's
    GX2DrawDone): 15-22 ms a frame boarding the boat with the island in view. Two hashes were half
    of it: each draw hashed its three shader programs byte by byte (now once a frame per program:
    `ProgramHash`, the same values), and each texture used was hashed whole every frame to see if it
    changed (in real time now a sample of each, its ends and 64 words, every frame, and whole
    textures in turn, at most 4 MB a frame and each every 30 frames or when its sample changes; a
    change the sample misses shows within a fraction of a second; with the virtual clock or
    `WWHD_TEXTURE_HASH=whole`, every texture whole every frame as before). 8-10 ms now.
  * *Pacing by pairs* (`src/overrides/pacing.cpp`): in real time at 60 a whole tick's frame that
    has missed its vsync doesn't wait for the next, and the half tick's frame waits for the pair's
    second vsync; flips aren't waited for (GX2DrawDone has the GPU done with the frame). A tick keeps
    its 33.3 ms whenever its two frames fit together, whichever overruns; a pair that overruns both
    slows the game by the overrun, not by a vsync. `WWHD_60FPS_PACING=vsync` keeps the game's wait.
    With the virtual clock the game's own wait runs (frames always fit there).
  * Sail route on the desktop, headless (frames 1801-2400, boarding with the island in view): 53.3
    fps before, 57.4 with pairs, 60.1 with the journal and renderer fixes too (no frame late; the
    work 9-13 ms, the renderer's thread 8-10 ms at its heaviest, 3.4 on average).
* **The sail flickered: it moved at 30.** Consecutive captures while sailing: the sail's change
  between frames alternated 6.1/2.6/5.5/2.3 (the wake's 11/22/14/26), the boat's steady. The sail
  is the cloth `GRID` (171, `daGrid_c`); `SAIL` (172) is a pirate ship's. Its `_execute` chases its
  alpha by 5 a tick and `ho_move` shapes the cloth from two phases advanced each tick, an
  approach, a countdown and the HD port's own flap (a 15-tick countdown, +0x2B8C). Rules: the
  phases' increments split between the half ticks, the countdowns and the alpha on whole ticks;
  `cLib_addCalc2`, `cLib_addCalc0` and `cLib_addCalcAngleS2` are converted already.
  `WWHD_60FPS_CONVERT=...,171`.
* **Particles** (`WWHD_60FPS_PARTICLES=1`). The boat's wake, bow waves and splashes are JParticle
  emitters with the ship's callbacks; the 3D particle calc (`dPa_control_c::calc3D`, `f_025A81A0`,
  JPAEmitterManager::calc on groups 0-6) ran on whole ticks. Now an override replaces its tick rule:
  whole ticks only, or with particles converted every frame with a step of h, its stores standing.
  JParticle (as the GameCube's, with an HD deletion delay) keeps its frame counters as floats, so a
  half step adds 0.5 exactly: emitter ticks, start frames, the rate step's timer (emission keeps a
  tick's cadence), particle ages; and `*h` on gravity, the fields' velocities (`calcVel`, types 0
  and 1), positions, `d` on air resistance and drag, `split` on rotation, children on whole ticks
  (where ages are the 30-tick run's). Census (`particles.txt` with the probe): while sailing, 606
  particles a whole tick at 30 against 595 at 60. Two strips are drawn through their particles, the
  wake (`dPa_trackEcallBack`, a triangle strip through each three) and the bow waves
  (`dPa_waveEcallBack`): their newest particles are the strip's end at the boat, so they emit every
  frame at a frame's count (`calcCreatePtcls`, `f_0281F878`; twice the particles, the same strip
  through twice the points; the wake's buffers hold 150 segments, 80 used). The bow waves' speed is
  the emitter's move per step, read per tick (`/h`). The step-doubling trial now runs any per-tick
  function (`WWHD_60FPS_TRIAL_PARTICLES=1`); for particles it mostly reports pooled particles
  recycled differently, and children's first velocity a half step apart, so captures are the
  measure.
* **The wake's texture scrolled once a tick.** Converted, the wake still changed only every other
  frame in captures: each half tick's frame matched the whole tick's after it, then a jump. The
  renderer's trace (now with hashes of what each draw reads, `| data u .. v .. vbufs .. vs-reads`,
  and `WWHD_RENDER_TRACE_VS=key` for one shader's constants and vertices) showed its 79 segment draws
  read new vertices and a new view on every frame, and skipping them (`WWHD_RENDER_SKIP_VS`, a
  probe) left the area still: the strip moves with the boat, as the camera does, so what showed on
  screen was its texture, scrolled by one of its two texture matrices only once a tick.
  JParticle's `JPADrawExecSetTexMtx` (the emitter's, `f_02832604`, and a particle's, `f_02830814`)
  takes `int tick = getFrame()` (or `getAge()`): with half steps the frame is .5 between ticks and
  the truncation moved the scroll on whole ticks only. The generator's new `reload:fD=rB+O[+O2]`
  rule puts the float frame back after the conversion. These run in the draw pass, where the step
  is 1, so it is conditioned on `g_rtSixty` (set while the game runs at 60; at 30 the frame counts
  are whole and the truncation changes nothing). The wake's tiles now change every frame alike
  (alternation 1.0-1.3, was 5-70). Ruled out on the way, each with byte-identical captures: the
  rollback, the hidden globals (`WWHD_60FPS_HIDE=0`, a probe), sead's nodes, fapGm_After, the
  GamePad's screen.
* **The sea isn't converted.** WWHD makes the sea's waves on the GPU: the height table its execute
  makes (`daSea_packet_c::execute`, four tick counters) stays flat (1.0) all along the sail route,
  open sea too, so the wake's points sit at a constant height. Its draw advanced the ripple
  texture's scroll every draw, twice as fast at 60 converted or not: now on whole ticks (a tick
  rule).

## Milestones

There are two tracks. They meet at M4.

**Step 3, the second round (2026-10-02/03).** The owner's camera overshoot (shield, sidling, ladders:
the camera swung far past Link's back and came back) was the shield camera, `f_02513E98` ('SHLD'),
an engine the routes had never reached: it approaches by (N - m11C) / W, W losing N - m11C a tick.
With no rules W lost it on both frames, the factor passed 1, and the camera overshot. A camera
engine's ramp reads tick counters (Run's, added to after the engine) and changes its weight in the
same tick; with the counters on whole ticks a half frame saw the next tick's count, so
`1 / (N - count)` ramps ran a frame ahead (the first-person camera's arrived a tick early,
front-loaded). A new tick rule, `late` (`RT_LATE_TICK()`: the half tick while the process steps,
the whole tick otherwise), puts such counters and weights at the tick's end; both frames of a tick
see its count, and `k` on the factor makes two half frames one tick's approach exactly: at 60 the
half frame lands on the 30-tick run's view. Counters added to before their use stay `whole`. Found
with a flight recorder (`WWHD_FLIGHT`: the tracked processes and the pad, the last 20 s, F9 writes
them) in the owner's play, then reproduced from the owner's save on the worker. The owner's
flickering torchlight was the real shadows: the HD renderer draws the shadow map only in a frame
whose environment update asked for it (`f_0255E854`), held to whole ticks, so whole frames drew a
placeholder. The update now runs on half ticks as well; it changes only globals and the random
stream, which a half tick's rollback puts back, so it reaches that frame's drawing and nothing else.
Frame-level work the renderer depends on belongs on every frame when the rollback can undo it.
What Link holds (his actor keeps: the arrow on the bow, a carried pot, bomb or rock) isn't
converted, so its matrices stayed a half tick behind his hands: on a half tick each held actor's
execute runs again as at 30 just before its draw, its changes put back right after that draw (and
its draw's stores journaled), so it is drawn at his hands and the game's state is the 30-tick
run's (checked: 0 state differences). A cheap pattern for anything attached to a converted process.
Events: converted processes held to whole ticks in every event (event code wasn't converted). Now
they step at 60 in an event while Link's current action is on a list of actions checked in events
(wait and move so far: an entrance's walk-out matches the 30-tick run); the rest (the Wind Waker's
conducting, whose beat counts ticks) stay at 30 until converted, action by action. The scene's
plants (grass, trees, bushes, flowers) are packets the play scene updates, not processes: their four
updates are overrides now, running every frame with a half step as the particle calc does.

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
| G3 ✅ (routes) | The route | Every scene on the scripted route is within tolerance. |

**Together**

| # | Milestone | Done when |
|---|---|---|
| M5 | Playable | The native CPU plus the native GX2 on a GPU machine play through the scripted route and beyond at full speed. |
| M6 | First enhancement | Native 60 fps (the owner's choice, 2026-10-01; not interpolation), then an uncapped frame rate: D21. |

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
`tools/recomp/build.sh` compiled them on the worker in about 4 minutes with 8 jobs (since WW-3 the
CMake build compiles them). Totals:
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

**G3 status (2026-10-01):** done on the three scripted routes. Captured every 60 frames on both
sides (lavapipe, virtual clock) and compared with `compare_frames.py --threshold 60`: the whole title
route (f60-f10800: title, file select, name entry, the legend, Aryll, Outset) 180 of 180 frames, worst
74.3 dB, 133 byte-identical; the save route (the dock from the 100% save) 30 of 30, worst 91.1 dB; the
tour route (the dock, then the pause menu) 36 of 36, worst 74.6 dB, the menu frames byte-identical.
Two reference runs differ from each other by about as much (73-75 dB).
* **What it took: the reference.** Before, every frame from Aryll's scene on (42 of the title route's,
  and all of the dock's) was at 53-60 dB: specks on silhouettes, 0.01-0.06% of pixels, the reference
  brighter. Surface dumps at the same swap (`compare_dumps.py`, with a fresh reference dump: the
  old one predated 0013) matched to the byte up to the scene buffer, and a frame trace
  (`WWHD_RENDER_TRACE=N:0`, every draw with its targets and textures) showed the last pass:
  antialiasing that reads the scene buffer `f5807800` and a luma mask while drawing into
  `f5807800`, blending edge pixels with their neighbours. The renderer samples a copy from before
  the draw; Cemu sampled the target in place (`VK_EXT_attachment_feedback_loop_layout`), whose
  reads of what the draw itself writes Vulkan leaves undefined, and lavapipe answered them in its
  shading order, so some edges were blended with neighbours already blended. A pixel probe on such
  a speck showed the renderer never writing it in that pass while the reference blended it. Cemu
  patch 0014 makes the reference read a copy too (the draw renders into a scratch copy of the target,
  copied back after it), on the evidence that the console's colour and texture caches are separate,
  so its texture reads see the image from before the pass; the dock went from 55-61 dB to 91-108 dB.
  The reference's OS-call trace is unchanged (172,954,163 calls on the save route).
* **Not on these routes yet** (so untested): GPU-side `GX2CopySurface` (only reported), readback into
  linear-special destinations, 3D textures and cube-map targets, depth-stencil textures loaded from
  memory. One address and format at several sizes in a frame is on the routes (the bloom blur's
  scratch target at 240x135, 120x67 and 60x33, `f41a8800`): each reader sees the right part.
* **Baselines** (worker): `/wwhd/data/g3/route-ref14`, `save-ref14`, `tour-ref14` (reference, with
  0014) and `route-vk`, `save-vk`, `tour-vk2` (the renderer).

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
`wwhd-null` was linked with its archive members in `wwhd`'s order (`src/link_order.py`); since
2026-10-01 its slots take the reference's addresses from a table instead (D18), and any link order
does.

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
