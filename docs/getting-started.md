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
