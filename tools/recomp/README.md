# tools/recomp - the static recompiler

Python generator (docs/recompiler-design.md D1, D11) plus the C++ runtime header it targets.

| File | What |
|---|---|
| `ppc.py` | Espresso decoder: `decode(word) -> Insn` with base mnemonics and fields |
| `census.py` | decodes every instruction in `config/US_v0/functions.csv`; mnemonic counts, `--csv` |
| `emit.py` | C++ for one instruction, semantics mirroring Cemu's interpreter; control flow via a `flow` policy |
| `runtime/ppc_ops.h` | memory, CR and FP helpers the emitted code uses (includes Cemu's headers) |
| `fuzz/` | M1 instruction fuzzer: emitted code vs `PPCInterpreterSlim_executeInstruction` |

## Fuzzer

On the worker (it links against the worker's Cemu build, so it never runs on the editing machine):

    tools/worker/job start census python3 tools/recomp/census.py orig/0005000010143500_v0/code/cking.rpx --csv /wwhd/data/census.csv
    tools/worker/job start fuzz-build env FUZZ_OPS=/wwhd/data/census.csv tools/recomp/fuzz/build.sh
    tools/worker/job start fuzz build/fuzz/fuzz 200        # iterations per encoding; optional MNEMONIC

`gen.py` synthesises random encodings (no game data); `harness.cpp` runs each from random
registers and a random 64 KB memory window through both sides and compares everything. Any NaN
equals any NaN in FP registers (counted in the summary). Generated cases go to `build/` only.
