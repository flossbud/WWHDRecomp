# WWHDRecomp

A native static recompilation of *The Legend of Zelda: The Wind Waker HD*
(Wii U, USA v0, `cking.rpx`), with targeted decompilation for enhancements, built
incrementally with AI-agent assistance.

**This repository contains no game code or assets.** Supply your own legally obtained
dump; see [`orig/README.md`](orig/README.md).

Status: getting started. See [`docs/getting-started.md`](docs/getting-started.md) for the
plan and [`docs/research/`](docs/research/) for the background survey.

## Tools

| | |
|---|---|
| `uv run tools/wua_extract.py GAME.wua orig/ code/` | extract the executable from a Cemu `.wua` and verify its hash |
| `python3 tools/rpx_info.py orig/…/code/cking.rpx [--sources]` | sections, imports, relocations, assert source-file names |
| `tools/ghidra/rebuild.sh` | rebuild the disposable Ghidra project (import, seed functions from relocations, apply `symbols.csv`) and refresh `config/US_v0/functions.csv` |
| `uv run tools/ghidra/apply_symbols.py` | apply `config/US_v0/symbols.csv` to the Ghidra project (idempotent) |

Ghidra setup: JDK 21, Ghidra **12.0.x** in `~/opt/ghidra_12.0.4_PUBLIC` (or set `GHIDRA_INSTALL_DIR`),
with [Maschell/GhidraRPXLoader](https://github.com/Maschell/GhidraRPXLoader) v0.9.2 unzipped into
`Ghidra/Extensions/`.

## Data files

| | |
|---|---|
| `config/US_v0/functions.csv` | every function Ghidra knows (address, size, name, source): generated, the recompiler's input |
| `config/US_v0/symbols.csv` | reviewed names with evidence; the source of truth applied into Ghidra |

Names in `symbols.csv` tagged `twwhd-randomizer@…` come from
[SuperDude88/TWWHD-Randomizer](https://github.com/SuperDude88/TWWHD-Randomizer) (MIT).
