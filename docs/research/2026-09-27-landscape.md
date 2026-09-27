# Landscape survey — 2026-09-27

Answers to the open questions in the original project handoff. Facts marked
**(verified)** were checked against the dump or via the GitHub API on this date;
everything else was reported by web research and is worth re-checking before
relying on it.

## 1. What our dump actually is (verified)

| | |
|---|---|
| Container | Cemu `.wua` (ZArchive, zstd, 64 KiB blocks) — read by `tools/wua_extract.py` |
| Title | `0005000010143500_v0` — USA, `WUP-P-BCZE`, **base game v0, no update** |
| SDK | `sdk_version` 20911, `os_version` 000500101000400A |
| Executable | `code/cking.rpx`, 6,994,496 bytes, sha256 `c4f0ab30…62f16153` (matches cemu-project/title-checksums) |
| `.text` | `0x02000020`, 0x8f87d4 bytes (~9.4 MB of code) |
| `.rodata` / `.data` / `.bss` | `0x10000000` / `0x1018c0c0` / `0x101fce00` |
| Symbols | **stripped** — only section markers + ~430 OS imports (coreinit, gx2, snd_core, vpad, padscore, nn_*) |
| Relocations | ~309k kept: ADDR32 73,780 · ADDR16_LO 117,609 · ADDR16_HA 107,890 · ADDR16_HI 27 · REL24 3,660. No GHS-specific reloc types in the main module. |
| Source-file strings | 462 distinct `*.cpp` names from asserts; 319 are `d_a_*` actors |

Things the strings tell us:

* The GameCube codebase survived intact at the file level: `d_a_*.cpp` actors,
  JSystem (`J2D*`, `J3D*`, `JAI*` audio, `JUT*`, `JFW*`), `mDoExt_*`, `dSv_*`,
  `dGrass_packet_c`, etc. — the same translation units `zeldaret/tww` has.
* Build path `D:/home/Cafe/cking/DevEnv/{jsys,sead}/…`: HD links Nintendo EPD's
  **sead** and **agl** libraries, plus NintendoWare (`ut_Print_Cafe.cpp`, `Lyt`).
* HD-only code lives under a `cking::` namespace (`cking::system::ErrorViewerTask`,
  `cking::ui::input::EventMgr`, `cking::system::OliveCommentMgr` for Miiverse).
* Relocations make every absolute pointer in code/data known, which is the hard
  part of both delinking and static recompilation.

## 2. Ghidra tooling

* **Use `Maschell/GhidraRPXLoader`**, not `cemu-project/GhidraRPXLoader` (a 2019
  fork). Latest v0.9.2 (2026-01-13) ships a **Ghidra 12.0** build only **(verified)**.
  Ghidra latest is 12.1.4 (2026-09-21, JDK 21). Either run Ghidra 12.0.x or
  rebuild the loader against 12.1.x with `gradle -PGHIDRA_INSTALL_DIR=…`.
* The loader bundles its own `PowerPC:BE:32:Gekko_Broadway_Espresso` language and
  decompresses zlib sections itself — no `wiiurpxtool` step.
  `aldelaro5/ghidra-gekko-broadway-lang` is dead (last push 2022), merged into
  `Cuyler36/Ghidra-GameCube-Loader` (1.3.1, Ghidra 12.1) — only needed if we
  also load the GameCube DOL/RELs, which we will for cross-referencing.
* Forks of interest: `nanax74/GhidraRPXLoader-PSQ-Fix` (paired-single load/store
  fix), `Maximal98/GhidraRPXLoader-GHSDemangler` (GHS C++ demangling; behind upstream).
* `Luminyx1/CXXAnalyzer` — Ghidra script reconstructing C++ classes from GHS Wii U binaries.
* Not usable: decomp-toolkit (no Wii U, per maintainer, issue #66); IDA has
  `decaf-emu/ida_rpl_loader` if ever needed; no Binary Ninja RPX loader.

## 3. Existing Wind Waker HD work

Nobody has a real WWHD decomp. Relevant pieces:

| Project | What it gives us |
|---|---|
| **SuperDude88/TWWHD-Randomizer** (MIT, US v0) | `asm/linker.ld`: ~111 named WWHD addresses in GC-decomp naming (`dSv_save_c_init = 0x025b9938`, `execItemGet = 0x0254da38`, `item_func_*`) **(verified)**; ~27 patch files; RPX/SARC/BFRES tooling. Best existing address source. Discord has a Ghidra DB (unverified whether shared). |
| **BlackLineInteractive/nWiiURecomp** (pushed 2026-08-05, 9★) | Static RPX→C++ recompiler; WWHD **EU** v0 `cking.rpx` is its one validated title, running through startup **(verified in README)**. Has `NWIIU_DIFF` block-vs-interpreter checking and a Ghidra profile exporter. |
| SomeoneIsWorking/setsail + wiiuport | Cemu fork running WWHD at 60 Hz by interpolation; `docs/title-identity.md` has measured facts. |
| Ori-Jakob/libwwhd, wwhd-tools, wii-u-rpl-loader | WWHD headers in GC-decomp layout; Aroma plugin hooking by address. |
| Arulo165/WWHD-Decomp | ~20 hand-written headers, depends on open-ead/sead. A stub. |
| cemu_graphic_packs | RPX module hashes (US `0x475BD29F`), a few code patches with addresses. |

Library decomps we can lean on:

* `zeldaret/tww` — GC Wind Waker, **78.6% decompiled / 65.8% linked**, full symbol
  maps for GZLE01/P01/J01 (dtk-template layout). Primary naming source.
* `aboood40091/sead` — sead as used on **Wii U** (NSMBU `red-pro2`, non-matching).
* `open-ead/sead`, `open-ead/agl` — Switch-era sead/agl from the BotW decomp.
* `zeldaret/tp` has no TP HD support (issue #3169 unanswered) — no help there.

## 4. Matching is less impossible than the handoff assumed

decomp.me added a **Wii U platform with GHS 5.3.22** (`cxppc.exe` under wibo),
merged 2025-12-07 **(verified: PR #1753)**. The compiler tarball is in
`decompme/compilers`. Our SDK is 2.09.11 (2013). Whether WWHD was built with that
exact GHS release is unknown, and no Wii U project has achieved matching yet
(red-pro2 declares it impossible) — but a short spike compiling a few small
leaf functions is cheap and would tell us whether objdiff-style scoring is on
the table.

## 5. Static recompilation landscape

N64Recomp/Zelda64Recomp, XenonRecomp (Unleashed Recompiled), ReXGlue (Xbox 360,
v0.10 2026-08), PS2Recomp, Wiicompiled (MKWii, 2026-08), `sp00nznet/ww` (GC Wind
Waker recomp, doesn't boot through its state machine yet). For Wii U: only
nWiiURecomp has real code. Espresso is easier than Xenon in some ways (32-bit,
no VMX128, relocations present, OS boundary is clean named RPL imports that
Cemu/decaf already implement) and harder in others (paired singles + GQR
quantized loads, 3 cores, GX2/Latte shaders — the biggest cost).

## 6. AI-assisted decompilation — what has worked

* Chris Lewis, Snowboard Kids 2 (N64, 100% on 2026-05-17): headless `claude`,
  **one function per invocation**, a scorer choosing the next function, attempt
  caps, defensive CLI tools with loud errors, commit after every success.
  "Defensive tooling beats prompt engineering."
* Klonoa GBA (2026-08): loop in `.claude/commands/*.md`, adversarial review
  subagents, parallel agents in worktrees; watch for the model "cheating".
* banteg's Crimsonland rewrite: `name_map.json` of renames **with evidence**,
  re-applied by script to regenerated decompiles; Frida traces → test fixtures.
* Quesma: models are "very eager to say something is the same" — use an external
  oracle (trace diff, screenshot diff), never the model's own judgement.
* Community norms: zeldaret/tww and Dusklight reject primarily AI-generated PRs;
  zeldaret/botw now has AGENTS.md/CLAUDE.md requiring disclosure + human review.
  Relevant if this is ever shared.

## 7. Ghidra + agents on a headless server

| Server | Headless | Notes |
|---|---|---|
| clearbluejar/pyghidra-mcp | yes (built for it) | persistent project, decompile/xrefs/rename/retype; no struct editing |
| cyberkaida/ReVa | yes (`mcp-reva`) | Ghidra ≥12.0; structs, vtables, types; Claude Code plugin |
| bethington/ghidra-mcp | yes (jar) | largest toolset, Ghidra Server integration; pinned to 12.1.3 |
| mrphrazer/ghidra-headless-mcp | yes | also a plain CLI with JSONL batch mode |
| LaurieWired/GhidraMCP | **no** (GUI) | stale since 2025-06 |

Whatever server is used, the durable state should be **text in git**
(symbols CSV, C headers) applied to a disposable Ghidra project by an
idempotent PyGhidra script. Ghidra project files are binary and not reviewable.

Porting names GC → HD: exact-bytes matchers (Function ID, VT exact correlators)
won't fire across CodeWarrior→GHS. What should work: string/assert anchors,
data-reference correlators, call-graph propagation from anchors, BSim to rank
candidates, agent/human confirmation side by side. Expect HD-only code
(GamePad, `cking::`, rendering) to have no GC counterpart.

## 8. Behaviour verification options

* **Cemu GDB stub** (`--enable-gdbstub`, port 1337): break on a function, record
  inputs/outputs → fixtures to test rewritten C against. Works on this server.
* **Aroma + libfunctionpatcher** on a real Wii U: `REPLACE_FUNCTION_OF_EXECUTABLE_BY_ADDRESS`
  swaps one function for compiled C in the running game. Needs hardware.
* **Recompiled baseline** (nWiiURecomp-style): swap hand-written functions into a
  recompiled whole program; `NWIIU_DIFF`-style differential checks.
* `boricj/ghidra-delinker-extension` does **not** support PowerPC.
* Cemu graphic-pack patches are assembly-only; not a practical C injection path.

## 9. Dumping

Already done: the `.wua` is Cemu's native archive (produced by Cemu's title
manager or by Dumpling on a Wii U). It contains only v0. The randomizer and
nWiiURecomp both target v0, so stay on v0 unless an update turns out to matter.
