# tools/recomp - the static recompiler

Python generator (docs/recompiler-design.md D1, D11) plus the C++ runtime header it targets.

| File | What |
|---|---|
| `ppc.py` | Espresso decoder: `decode(word) -> Insn` with base mnemonics and fields |
| `census.py` | decodes every instruction in `config/US_v0/functions.csv`; mnemonic counts, `--csv` |
| `emit.py` | C++ for one instruction, semantics mirroring Cemu's interpreter; control flow via a `flow` policy |
| `runtime/ppc_ops.h` | memory, CR and FP helpers the emitted code uses (includes Cemu's headers); stores go through the diff-mode journal hook |
| `runtime/recomp_tables.h` | layout of the tables generated next to the code (functions with purity, code hash and callees; imports; import sites; store census), read by `src/runtime` |
| `fuzz/` | M1 instruction fuzzer: emitted code vs `PPCInterpreterSlim_executeInstruction` |
| `generate.py` | M2: every function as C++ (gotos, musttail calls, jump-table switches, imports, D7 helper entries) into shards |
| `build.sh` | M2: generate and compile the whole program on the worker (`build/recomp`, never committed) |

## Whole program (M2)

    tools/worker/job start recomp-build tools/recomp/build.sh

Generates `build/recomp/shard_*.cpp`, `funcs.h`, `func_table.cpp` and `imports.cpp`, then compiles
them with Cemu's flags plus `-ffp-contract=off -fno-strict-aliasing`, against a precompiled
`runtime/ppc_ops.h`. The build is incremental: only shards whose text changed recompile. The
runtime functions they call (`rt_import`, `rt_import_data`, `rt_call_ctr`, `rt_jump_ctr`,
`rt_bad_branch`, ...) are in `src/runtime`, which links it all into `wwhd-null` (M3).

What the generator knows beyond single instructions:

* **Purity (D8.2).** A function is pure if it makes no import call, no indirect call or jump and
  no weak call, and calls only pure functions: 12,968 of 39,720.
* **Jump tables.** All 294 of WWHD's are runs of `b` instructions that the `bctr` jumps into, so
  the switch cases on the slot addresses (table + 4k); each slot's `b` then jumps on. The shape is
  read from the code, not from `jump_tables.csv`'s `bound` column (which says how the bound was
  found). Until M3 the switches cased on the final targets and could never match.
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
