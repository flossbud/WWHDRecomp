# Getting started — plan

Evidence behind each step is in [`research/2026-09-27-landscape.md`](research/2026-09-27-landscape.md).

## Goal and approach

**End goal: a native static recompilation of Wind Waker HD** (US v0 `cking.rpx`), in the
style of Zelda64Recomp and Unleashed Recompiled.

A recomp does not need a decomp first. The original PowerPC is translated mechanically
into C and linked against a native runtime. Reverse-engineering effort goes into:

1. **Recompiler inputs.** Function boundaries, jump tables and relocations. The RPX keeps
   about 309k relocations, which helps a lot.
2. **The runtime.** Cafe OS library HLE, GX2/Latte graphics, audio, input, and three CPU
   cores. This is the biggest cost. The realistic route is reusing Cemu's
   implementations (MPL-2.0) rather than writing them from scratch.
3. **Targeted decompilation**, only for functions we want to change: frame interpolation
   (60 fps), widescreen/UI, input, bug fixes. The GameCube `zeldaret/tww` names are
   what make this tractable.

Decompiling whole systems into readable C is a later, optional layer. Recompiled
functions can be swapped out for hand-written ones one at a time, and the recompiled
original then serves as the oracle for them.

## Phase 0 — evaluate nWiiURecomp (1–2 days)

`BlackLineInteractive/nWiiURecomp` is the only Wii U recompiler with real code, and WWHD
EU v0 is its one validated title (runs through startup). Build it on this server, write a
profile for **US v0**, and record in `docs/research/` how far it gets.

Answer these questions:

* Does the translator handle all of `.text`, including paired singles and GQR-quantized
  loads, jump tables, and GHS prologues?
* What does its runtime do for GX2, OS threads/cores, and filesystem access?
  How much of it is Cemu?
* What is the code quality, test coverage (`NWIIU_DIFF`), and single-author bus factor?
* Licence: the README says GPL-3.0, but the `LICENSE` file contains the text
  "404: Not Found". Confirm with the author before building on it.

**Decision after Phase 0:** fork or contribute to nWiiURecomp, or write our own
recompiler in the XenonRecomp style. Either way, the runtime is built on Cemu code.

**Outcome (2026-09-27, [evaluation](research/2026-09-27-nwiiurecomp-eval.md)):**

* After a one-line fix, the lifter translates all of US v0, and the result compiles and
  runs. Both the recompiled program and the interpreter stop at the same HLE fault after
  6.2M instructions.
* The output is basic-block granularity, and the playable Cemu port and the LLVM trunk
  are not public.
* Recommendation: write our own function-level recompiler, and use nWiiURecomp as a
  reference and cross-check.

Build dependencies installed on this server: `cmake ninja-build pkg-config libssl-dev
zlib1g-dev libshaderc-dev libsdl3-dev`.

## Phase 1 — Ghidra baseline (parallel track, about 1 day)

Server state today: no Java/Ghidra/cmake and **no GPU**; Python 3.13 and uv are present;
4 cores, 10 GB RAM, about 25 GB free disk. Building and codegen can run here. Running the
game needs a Vulkan-capable machine.

1. Install JDK 21 and **Ghidra 12.0.x**, matching `Maschell/GhidraRPXLoader` v0.9.2,
   which is built for 12.0 only.
2. Import `orig/…/code/cking.rpx` headless into `ghidra/projects/` (gitignored).
3. `tools/ghidra/export_functions.py` (PyGhidra) writes `config/US_v0/functions.csv`
   (address, size, name, status). This is the function list the recompiler consumes.
4. `tools/ghidra/apply_symbols.py` applies `config/US_v0/symbols.csv` back into Ghidra
   idempotently.
5. Check paired-single decoding. If `psq_*` looks wrong, try the `nanax74` PSQ-fix fork.

**Outcome (2026-09-27): done.**

Setup:

* Ghidra 12.0.4 and JDK 21 are installed in `~/opt`.
* The v0.9.2 loader zip (declared version `12.0`) loads on 12.0.4 without a rebuild.
* It selects `PowerPC:BE:32:Gekko_Broadway_Espresso`.

`tools/ghidra/rebuild.sh` rebuilds the project from scratch in about 7 minutes, and two
from-scratch runs produce byte-identical `functions.csv` and `jump_tables.csv`.
`tools/audit_functions.py` checks the result against facts in the RPX itself. Every one
of its checks is currently zero.

| Step | What it fixes |
|---|---|
| Import + auto-analysis | 29,384 functions covering 97.4% of `.text`. |
| `tools/jump_tables.py` | Finds **294 GHS switches (5,243 cases)**; see below. Ghidra can't recover these. |
| `apply_jump_tables.py` | Adds computed-jump references from each `bctr` to its table, deletes functions wrongly started at table entries or case labels, and writes decompiler switch overrides. |
| `seed_functions.py` | Starts a function at every relocation target in `.text` (address-taken code: vtables, callbacks, `lis/addi` pairs), with switch tables excluded. About 10,460 new functions. |
| `split_functions.py` | Normalises function boundaries using GHS's layout rules; see below. Converges in 2 passes. |
| `apply_symbols.py` | Applies the 110 randomizer names. All 97 function names land on existing entries. |
| **Result** | **39,705 functions in `.text`, covering all but 524 bytes** (164 of them padding, the rest dead prefixes of the register save/restore helpers). There are also 376 OS import stubs outside `.text`. |

### What GHS code looks like (and what the recompiler must handle)

* **Switches are branch tables in code.** The switch computes `table + 4*i` with `lis` +
  `addic`/`addi` and `bctr`s into a run of `b case_i` instructions placed right after the
  `bctr`. The bound is a `cmplwi` (266 switches), sometimes in a predecessor block. For
  the other 28 we fall back to the length of the `b` run; every count is checked against
  "the first case starts right after the table".
* **Functions are contiguous and start at their entry.** The split pass enforces this:
  * Any detached range Ghidra attached to a function is really a *tail-called* function.
    The sources are switches whose cases are tail calls, jumps through `.rodata` pointer
    tables, and `b` to a lone `blr` (an empty function).
  * Code before the entry is a function it tail-calls.
  * The exception is **loop rotation** (`b test` / loop / `test: … bne loop`) at the start
    of a function. Ghidra calls the `b` a thunk, and we merge it back.
* **Any address-taken code inside a body is a function entry.** For example, the
  pure-virtual stub `li r3,13; b abort` referenced by thousands of vtable slots sits right
  after a no-return call, with no terminator in between.
* **Register save/restore helpers** at `0x028F5EE0` to `0x028F626C` are straight runs of
  `stw`/`lwz`/`stfd`/`lfd`, one entry per register. Prologues call into them part-way
  (`bl`), and epilogues tail-branch into them (`b`). Each entry falls through into the
  next, so the recompiler needs to special-case them, as XenonRecomp does with
  `__savegprlr_N`.
* **Callee-saved FPRs are full paired singles.** The save helpers store both halves
  (`stfd fN` → `ps_merge10 fN,fN,fN` → `stfs fN`). This is where 9,277 of the 13,398
  paired-single instructions come from.
* **Indirect control flow:** there are 14,947 `bctrl` (indirect calls, mostly virtual) and
  1,555 non-switch `bctr` (indirect tail calls, e.g. GHS virtual thunks). The recompiler
  needs a runtime address → function lookup, and every target must be a function entry,
  which is what the seeding and audit guarantee for address-taken code.

Checks:

* **Paired singles** decode correctly: 13,398 instructions in 3,432 functions.
* **Cross-check against `nwiiu-analyze`.** Nearly all function starts agree. Its extra
  entries are block-level (branch and case targets inside functions).

Still open:

* **Suspect decompiler output.** The float-heavy function `FUN_027e565c` decompiles with
  many "removing unreachable block" warnings. Check whether paired-single p-code
  semantics are the cause (see the PSQ-fix fork). This matters for reading the code,
  not for the recompiler.

## Phase 2 — symbol bootstrap (parallel track, 1–2 weeks)

In order of confidence, all recorded with evidence:

1. The TWWHD Randomizer's ~111 names in `asm/linker.ld` (MIT licence, US v0).
2. Assert-string anchors: functions referencing `"d_a_foo.cpp"` belong to that
   translation unit (462 source-file names).
3. GameCube → HD porting from `zeldaret/tww`, using unique strings, float constants and
   call-graph shape, with BSim to rank candidates.
4. Library names: sead (`aboood40091/sead`, `open-ead/sead`), agl (`open-ead/agl`), and
   JSystem (from `zeldaret/tww`).

## Phase 3 — first boot milestone

Get the recompiled game to its title screen on a GPU machine. The work depends on the
Phase 0 decision. The likely order is:

1. CPU-only: run through init with graphics stubbed, the `NWIIU_DIFF`-style differential
   check against an interpreter.
2. Graphics through Cemu's Latte renderer.
3. Input and audio.

## Phase 4 — enhancements and the agent loop

Start with **60 fps via tick interpolation** (setsail demonstrates it is viable). That
needs camera/actor transform code understood, which is where Phase 2 names pay off.

The agent loop (per the Snowboard Kids 2 and Klonoa lessons):

* One function per headless session, picked from a queue in `functions.csv`.
* Defensive, loud CLI tools.
* A function counts as "done" only when a differential check against the recompiled
  original passes.
* Commit after every success.

Ghidra access goes through a headless MCP server: pyghidra-mcp first, ReVa if we need
struct editing.

## Side experiment: matching

decomp.me has GHS 5.3.22 for Wii U. A one-day spike compiling a few leaf functions
would show whether byte-exact matching is possible. It is lower priority now that the
goal is a recomp.

## Hard rules (carried from the handoff)

* Never commit or generate `.rpx`/`.rpl`, extracted assets or other game data.
  `orig/` is gitignored, and users bring their own dump.
* Recompiler output generated from the game binary is also game-derived. Generate it
  at build time on the user's machine and never commit it. Wiicompiled does the same.
* Raw decompiler dumps stay out of git. Only hand-reviewed source goes in.
