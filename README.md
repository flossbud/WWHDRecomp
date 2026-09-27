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
