# Notes for agents

- Goal: static **recomp** of the game; decompile only what we need to change. See the plan.

- Target: WWHD **USA v0** `cking.rpx` (sha256 in `orig/README.md`). All addresses assume it.
- **Never** commit or emit game data: no `.rpx`/`.rpl`/`.wua`, no extracted assets, no raw
  decompiler dumps, no generated recompiler output. `orig/` is gitignored — keep it that way.
- Durable knowledge lives as text in git (planned: `config/US_v0/*.csv`, headers). Ghidra
  projects are disposable and rebuilt by script.
- Every rename/retype needs recorded evidence (string anchor, GC-decomp match, trace).
  A function is only "done" when an external check (fixtures/trace diff) passes.
- Plan: `docs/getting-started.md`. Background: `docs/research/`.
