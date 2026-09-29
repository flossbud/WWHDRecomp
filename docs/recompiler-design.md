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

 reference (not shipped): unmodified upstream Cemu run under Xvfb + lavapipe, which produces
 reference frames and GX2 call traces for the same scripted input
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

### D16. Graphics verification

* **The reference** is unmodified upstream Cemu, run under Xvfb with lavapipe, on the same
  scripted input.
* **Determinism.** Both builds run with a test-mode clock (`OSGetTime`/`OSGetSystemTime` advance
  a fixed step per frame) and frame-indexed input. The same frame number then shows the same
  scene. The reference Cemu needs a small patch for the clock; that patch is the only change to it.
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

## Milestones

There are two tracks. They meet at M4.

**Foundation**

| # | Milestone | Done when |
|---|---|---|
| M0a ✅ (core) | Reference | Upstream Cemu builds here and boots WWHD under Xvfb + lavapipe. The deterministic-clock patch and GX2 call logging produce frames and traces for the scripted route. |
| M0b | Runtime skeleton | Our frontend links Cemu's OS libraries **without Latte, gx2 or TCL**, plus our GX2 front half with the null backend. WWHD boots with Cemu's **interpreter**. Its GX2 call stream to the title screen matches the reference (D16.1). |

**CPU track**

| # | Milestone | Done when |
|---|---|---|
| M1 | Instruction semantics | The generator emits all 200 mnemonics. The instruction fuzzer passes against Cemu's interpreter. |
| M2 | Whole program compiles | All 39,705 functions generate with zero unknown instructions and zero unresolved branches, and compile with clang. |
| M3 | Pure functions native | `native_dispatch` is on for pure functions, with sampled diff mode clean over the scripted route (null backend). |
| M4 | Everything native | HLE calls direct, yield points in place, `GamePatch` handled. The GX2 call stream still matches the reference, and the interpreter fallback counter is 0. |

**Graphics track**

| # | Milestone | Done when |
|---|---|---|
| G0 | Trace | The D15 trace scopes the backend. |
| G1 | Shader corpus | Every program in the game files is extracted and translated to SPIR-V that passes `spirv-val`, and every program seen in the G0 trace is in the corpus. |
| G2 | First pixels | The title screen (TV) renders within tolerance of the reference on lavapipe. |
| G3 | The route | Every scene on the scripted route is within tolerance. |

**Together**

| # | Milestone | Done when |
|---|---|---|
| M5 | Playable | The native CPU plus the native GX2 on a GPU machine play through the scripted route and beyond at full speed. |
| M6 | First enhancement | 60 fps interpolation as overrides (needs Phase 2 names). |

**M0a status (2026-09-28):** the patched reference is deterministic. Two fresh boots traced to
frame 600 give 64.9M identical OS-call and scheduler records; see `tools/reference/README.md`.
Still open: scripted input for a deeper route, and hardware rendering in the worker.

The CPU track needs no rendering, and the graphics track can use Cemu's interpreter for the CPU
(M0b), so the two proceed in parallel.

## Open questions

1. **Cemu build footprint.** Can Cemu's OS libraries (CemuCafe minus Latte, gx2 and TCL), Config,
   Input, Audio and Util build without wx and without vcpkg's full dependency tree? M0b answers
   this, and the coupling shim (D12) is the known part.
2. **`GamePatch` and graphic packs.** Which Cemu game patches target WWHD US (D10)?
3. **Fiber stack size and `musttail`.** Is 2 MB enough for native recursion? Check during M3/M4.
4. **Floating-point exactness.** Does the host FPU plus correct rounding match Espresso for
   `fmadds` (fused vs. separate rounding) and denormals? The M1 fuzzer will show it; Cemu's
   interpreter documents its own choices.
5. **Shader state variants.** How many (program × fetch-layout × render-target-format) variants
   does WWHD really use? G0 answers this, and it decides specialisation constants versus
   enumerated variants.
6. **Geometry shaders, MSAA, HiZ, stream-out.** Which of them does WWHD use (G0)?
7. **Project licence.** Pick one before anything is published.
8. **GPU machine.** Which machine does M5 run on: your desktop, or a VM with GPU passthrough?
