# tools/recomp - the static recompiler

Python generator (docs/recompiler-design.md D1, D11) plus the C++ runtime header it targets.

| File | What |
|---|---|
| `ppc.py` | Espresso decoder: `decode(word) -> Insn` with base mnemonics and fields |
| `census.py` | decodes every instruction in `config/US_v0/functions.csv`; mnemonic counts, `--csv` |
| `emit.py` | C++ for one instruction, semantics mirroring Cemu's interpreter; control flow via a `flow` policy |
| `runtime/ppc_ops.h` | memory, CR and FP helpers the emitted code uses (includes Cemu's headers); stores go through the journal hook (diff mode, the fast paths' quiet watch) |
| `runtime/recomp_tables.h` | layout of the tables generated next to the code (functions with purity, code hash and callees; imports; import sites; store census), read by `src/runtime` |
| `fuzz/` | M1 instruction fuzzer: emitted code vs `PPCInterpreterSlim_executeInstruction` |
| `generate.py` | M2: every function as C++ (gotos, musttail calls, jump-table switches, imports, D7 helper entries) into shards |
| `build.sh` | M2: generate the whole program on the worker (`build/recomp`, never committed); `src/build.sh` compiles it |

## Whole program (M2)

    tools/worker/job start recomp-build tools/recomp/build.sh

Generates `build/recomp/shard_*.cpp`, `funcs.h`, `func_table.cpp` and `imports.cpp`. The CMake build
(`src/build.sh`, `src/CMakeLists.txt`) compiles them with CemuCafe's flags plus `-ffp-contract=off
-fno-strict-aliasing`, against a precompiled `runtime/ppc_ops.h`. It is incremental: the generator
rewrites only shards whose text changed, and only those recompile. The
runtime functions they call (`rt_import`, `rt_import_data`, `rt_call_ctr`, `rt_jump_ctr`,
`rt_bad_branch`, ...) are in `src/runtime`, which links it all into `wwhd-null` (M3).

What the generator knows beyond single instructions:

* **Guest time (D6).** Every instruction costs one cycle of the timeslice, and the thread yields in
  place (`rt_yield`) where it runs out, exactly where Cemu's interpreter would switch. Counted per
  basic block: a block of 3 or more instructions that fits in the slice is charged at once
  (`RT_FITS`/`RT_CHARGE`) and runs unchecked; otherwise a second copy with `RT_TICK(address)` before
  every instruction runs. Blocks end at branches, calls, imports and runtime hooks, so nothing in
  one can see the difference. `generate.py --tick instruction` emits only the checked form.
* **Indirect calls (D5).** `bctrl`/`bctr` go straight to the recompiled function when CTR holds
  the entry of one that runs natively (`RT_CALL_CTR`/`RT_JUMP_CTR`: `rt_direct`, one slot per guest
  code word, filled by the runtime), else to the runtime as before.
* **Helpers.** Cemu's `fcmpu_espresso` is inlined as `rt_fcmpu` (the fuzzer checks it: 3.6 million
  runs of `fcmpu`, `ps_cmpu0/1` and `ps_cmpo0`, 0 differences). Every generated function takes
  `PPCInterpreter_t* __restrict ctx`: guest memory never overlaps the register state.
* **Cemu's boot patches (D10).** `config/US_v0/code_patches.csv` (the words Cemu's GamePatch changes,
  with evidence) is applied first, so the generated code is the code Cemu runs.
* **Purity (D8.2).** A function is pure if it makes no import call, no indirect call or jump and
  no weak call, and calls only pure functions: 12,968 of 39,720.
* **Jump tables.** All 294 of WWHD's are runs of `b` instructions that the `bctr` jumps into, so
  the switch cases on the slot addresses (table + 4k); each slot's `b` then jumps on. The shape is
  read from the code, not from `jump_tables.csv`'s `bound` column (which says how the bound was
  found). Until M3 the switches cased on the final targets and could never match.
* **Overrides (D9).** A function listed in `config/US_v0/overrides.txt` is emitted as `orig_f_X`
  (declared in `funcs.h` too), and `src/overrides` defines `f_X`. Every reference names `f_X` (calls,
  tail calls, falling through, the function table and with it indirect calls), so all reach the
  override; the linker reports a listed function without an override, or an override of an unlisted
  one. Only the listed functions' shards change, and nothing is emitted weak, so inlining within a
  shard is untouched.
* **Imports.** 408 (the RPX's import-section symbols are not imports). Relocations are keyed by
  symbol: `_iob+0x10` (stdout) lands on `environ`'s stub address but is `_iob` plus an addend.

## Fuzzer

On the worker (it links against the worker's Cemu build, so it never runs on the editing machine):

    tools/worker/job start census python3 tools/recomp/census.py orig/0005000010143500_v0/code/cking.rpx --csv /wwhd/data/census.csv
    tools/worker/job start fuzz-build env FUZZ_OPS=/wwhd/data/census.csv tools/recomp/fuzz/build.sh
    tools/worker/job start fuzz build/fuzz/fuzz 200        # iterations per encoding; optional MNEMONIC

`gen.py` synthesises random encodings (no game data); `harness.cpp` runs each from random
registers and a random 64 KB memory window through both sides and compares everything. Any NaN
equals any NaN in FP registers (counted in the summary). Generated cases go to `build/` only.
