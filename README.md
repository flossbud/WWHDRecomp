# WWHDRecomp

A native **static recompilation** of *The Legend of Zelda: The Wind Waker HD* (Wii U), with **true 60 fps**.

The game's PowerPC code is translated to C++ ahead of time and runs natively on Linux, with a Vulkan renderer and
SDL3 for the window, sound and controllers. At 60 fps the game's own logic runs 60 times a second, and every actor
is checked frame by frame against the original 30 fps behaviour. It isn't frame interpolation: physics, enemies,
Link's moves, timers and cutscenes play as the original does, only smoother.

> **No game code or assets are included.** You need your own legally obtained dump of the game (USA, v0). This
> project isn't affiliated with or endorsed by Nintendo.

## Status

**Developer preview.** The whole game is playable on Linux (x86-64) at 30 and 60 fps on a desktop GPU. There's no
packaged release yet; you build it from source (below).

- **True 60 fps:** all ~450 actor types are converted and verified against the 30 fps original. What remains is a
  short list of known differences (a few fights drift by some units after a knockback, things decided by random
  numbers).
- **Verified, not guessed:** at 30 fps the recompiled game matches a deterministic reference emulator bit for bit:
  OS calls, GPU command streams and sound. Every change is checked against it.
- **Performance:** a recent desktop CPU holds 60 fps easily. Handheld-class CPUs (Steam Deck) don't yet; that's
  the current focus ([`docs/research/deck-plan.md`](docs/research/deck-plan.md)).
- **Not yet:** Windows, macOS, Android and arm64 builds; the GamePad's second screen (play is single-screen, Pro
  Controller style); other regions or versions of the game.

## Requirements

- Linux on x86-64
- Docker or Podman
- A GPU with a Vulkan driver (tested: Mesa RADV on AMD, ANV on Intel)
- About 60 GB of free disk space and 16 GB of RAM for the first build
- Your dump of *The Wind Waker HD*, USA v0 (title `0005000010143500`), as a Cemu `.wua` archive

## Installation (build from source)

The build runs inside a container, so you don't have to install compilers and libraries on your system.

**1. Get the source and build the container image**

```sh
git clone https://github.com/flossbud/WWHDRecomp.git
cd WWHDRecomp
docker build -t wwhd-worker tools/worker
```

**2. Start the container**, with a data directory for the tools, your game and the builds:

```sh
mkdir -p ~/wwhd/data/rom
docker run -d --name wwhd-worker --init --device /dev/dri \
    -v ~/wwhd:/wwhd -v "$PWD":/wwhd/WWHDRecomp wwhd-worker
docker exec -it wwhd-worker bash          # the rest runs in here, in /wwhd/WWHDRecomp
```

With Podman, use `podman` in place of `docker` and add `--userns=keep-id`.

**3. Install the build tools** into the data directory. This fetches Cemu at a pinned commit, applies this
project's patches (`tools/reference/cemu-patches/`), and builds SDL3. It takes a while the first time.

```sh
tools/worker/setup-volume.sh cemu
tools/worker/setup-volume.sh sdl3
```

**4. Add your game.** Copy your `.wua` into `~/wwhd/data/rom/` on the host. Then, in the container, extract the
executable; the tool checks it against the known hash (`c4f0ab30…` for `code/cking.rpx`, see
[`orig/README.md`](orig/README.md)):

```sh
uv run tools/wua_extract.py /wwhd/data/rom/*.wua orig/ code/
```

**5. Recompile and build.** The first build also compiles Cemu's libraries, about 15-30 minutes depending on your
CPU:

```sh
tools/recomp/build.sh && src/build.sh
```

This produces `build/wwhd/wwhd-null`, the game program.

**6. Play.** The program runs from a play folder that holds it, Cemu's resource files, your game and your
settings and saves. [`tools/play/deploy.sh`](tools/play/deploy.sh) shows the layout and fills it.
[`tools/play/play.sh`](tools/play/play.sh) starts the game (30 fps); `WWHD_60FPS=1 ./play.sh` plays at 60 fps.

> A one-step local installer (building the play folder on your machine) is being worked on. Until then, step 6
> needs some hands-on setup.

**Controls** (a gamepad works with its printed button labels; on the keyboard, the Pro Controller's buttons are):
A = X, B = Z, X = S, Y = A, L = Q, R = W, ZL = 1, ZR = 2, + = Return, − = Backspace, the D-pad on the arrow keys,
the left stick on I/J/K/L, the right stick on T/F/G/H. F11 or Alt+Enter toggles fullscreen, and F1 opens the debug
menu.

## How it works

- **`tools/recomp/`:** the recompiler. A Python generator turns every function of the game's `cking.rpx` into C++.
  The generated code is built locally from your dump and is never committed.
- **`src/`:** the runtime the generated code links against:
  - the OS layer (Cemu's OS libraries, with parts forked and replaced over time);
  - a null GPU that consumes the game's GX2 command buffers;
  - the Vulkan renderer;
  - the true-60 machinery (`src/overrides/sixty*.cpp`, with per-address rules in `config/US_v0/tick_rules*`).
- **`config/US_v0/`:** what's known about the game's code: functions, jump tables, and reviewed symbol names with
  their evidence.
- **`docs/`:**
  - the design: [`docs/recompiler-design.md`](docs/recompiler-design.md);
  - the plan: [`docs/getting-started.md`](docs/getting-started.md);
  - the working notes: [`docs/handoff.md`](docs/handoff.md);
  - research: [`docs/research/`](docs/research/).

Developer tools, for those working on the recompiler itself:

| | |
|---|---|
| `uv run tools/wua_extract.py GAME.wua orig/ code/` | extract the executable from a `.wua` and verify its hash |
| `python3 tools/rpx_info.py orig/…/code/cking.rpx [--sources]` | sections, imports, relocations, source-file names |
| `tools/ghidra/rebuild.sh` | rebuild the Ghidra project and refresh `config/US_v0/{jump_tables,functions}.csv` (Ghidra 12.0.x with [GhidraRPXLoader](https://github.com/Maschell/GhidraRPXLoader)) |
| `python3 tools/audit_functions.py [--list]` | check function boundaries against the RPX; every check should be 0 |
| `tools/sixty/tests/` | the checks: 30 fps against the reference, and 60 against 30 |

## Credits

- [Cemu](https://github.com/cemu-project/Cemu) (MPL-2.0): the OS libraries, the shader decompiler and the reference
  emulator. Files derived from Cemu keep its license.
- [zeldaret/tww](https://github.com/zeldaret/tww), the GameCube decompilation: names and structures.
- [SuperDude88/TWWHD-Randomizer](https://github.com/SuperDude88/TWWHD-Randomizer) (MIT): symbol names tagged
  `twwhd-randomizer@…` in `config/US_v0/symbols.csv`.
- Built incrementally with AI coding agents (Claude).

## License

[MPL-2.0](LICENSE), the same license as Cemu, which much of the runtime derives from. It covers this project's code
and tools only. The game itself, and everything generated from your copy of it, is not part of this repository and
not covered by it.
`src/third_party/fsr1/` is AMD's FidelityFX Super Resolution 1 (`ffx_a.h`, `ffx_fsr1.h`), under the MIT license in
`src/third_party/fsr1/LICENSE.txt`.
`src/third_party/fsr3/` is AMD's FidelityFX SDK v1.1.4 FSR 3.1 upscaler (host code, headers and shaders), under the
MIT license in `src/third_party/fsr3/LICENSE.txt` (its `README.md`: what's taken and the one local change).
