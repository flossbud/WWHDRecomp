# tools/shaders - the shader corpus (G1)

Every GX2 shader program the game can bind, translated ahead of time to SPIR-V
(docs/recompiler-design.md D14). Everything these tools write is derived from game data: it goes
to `build/` or the worker's `/wwhd/data`, never into git.

| File | What |
|---|---|
| `corpus.py` | walks the title's `content/` (Yaz0, SARC, SHARCFB v9, GFD `.gsh`, and GFD files embedded in BFRES), writes each distinct program once (`programs/<hash>.<vs\|ps>`: its GX2 registers and microcode) and `index.csv`; checks the programs a run used are all there |
| `translate.cpp` | Cemu's shader decompiler plus glslang, as Cemu's Vulkan renderer runs them, but offline: GLSL and SPIR-V for every program |
| `build.sh` | builds `build/shaders/translate` against the worker's Cemu (Cemu_release's link line with `--wrap=main`, like the M1 fuzzer) |
| `recipes.py` | counts the pipelines in the renderer's recipe file (`pipelines.bin`, D20), and how many more dynamic state would save |
| `shader_list.py` | merges what runs met for the first time (`WWHD_SHADER_SOURCES`) into the shader list, `config/US_v0/shader_list.txt.gz` (D20; .gz read and written by extension), and adds which content file holds each program (from `corpus.py`'s index) |
| `cover_runs.sh` | capturing runs, several at once, each from an empty cache: routes and stage-survey chunks (s-shadercover); `LIST=path` checks a list instead (first sights during play, per item) |
| `survey_route.py` | the stage survey: a `WWHD_DEBUG_STAGE` warp into every room that has a player spawn point and an archive, the camera turned round, split into chunks |

Programs are keyed by the FNV-1a hash of their microcode as the GPU reads it, the same hash the
null GPU's G0 statistics use (`WWHD_GPU_STATS`).

```sh
# on the worker: extract content/ once, then
tools/worker/job start g1-corpus uv run -q tools/shaders/corpus.py \
    /wwhd/data/orig/0005000010143500_v0/content /wwhd/data/g1 \
    --seen /wwhd/data/g0/gpu-route.txt.variants.csv --runtime /wwhd/data/g1/runtime
tools/worker/job start shaders-build tools/shaders/build.sh
build/shaders/translate corpus /wwhd/data/g1/programs OUT [K/N]     # every program in the files
build/shaders/translate variants /wwhd/data/g1/runtime OUT          # a run's variants (WWHD_GPU_DUMP)
spirv-val --target-env vulkan1.1 OUT/*.spv
```

Two ways to translate, because a program's GLSL also depends on draw state that isn't in its
microcode (texture dimensions, render-target number formats, the vertex fetch layout):

* **`variants`:** each variant a run dumped (`WWHD_GPU_DUMP=dir` in `wwhd-null`) with the whole
  register file of its first draw and its fetch, vertex and pixel programs. These are exactly the
  inputs the reference's decompiler had.
* **`corpus`:** every program in the files, with neutral state:
  * every texture unit is 2D, except units the microcode itself uses as cube maps
    (`SET_CUBEMAP_INDEX`; the decompiler only declares the cube index when the register says so);
  * render targets are float/UNORM;
  * vertex shaders get a synthesized fetch shader: a big-endian float4 per semantic in their
    table.

  This shows the translator handles every program. Rendering correctness is G2's check.

Status (2026-09-29), see the design doc's G1 status: 30,011 distinct programs from 99,006 in the
files, all translated, 0 invalid in `spirv-val`. Every program the scripted route uses (236 vertex,
268 pixel) is among them, and its 287 variants translate with their runtime state.

## The shader list (D20)

A first start without hitches: `config/US_v0/shader_list.txt.gz` (gzipped; Android still ships B90's plain
`shader_list.txt` until a first start of the full list is measured on the phones) holds, for every shader playthroughs
have met, its key, its program's hash and the content file it is in, its fetch shader and the
registers its translation read, and every pipeline recipe. No game content; the player's machine
translates the shaders from its own game files before the game starts. To add to it, capture runs
(an empty cache doesn't matter: a capturing run prepares nothing) and merge on the worker:

```sh
# a run anywhere (the worker, or headless on the desktop with play.sh): everything it meets is recorded
WWHD_SHADER_SOURCES=/tmp/sources.txt WWHD_SHADER_LIST=none ...
tools/worker/w python3 tools/shaders/shader_list.py config/US_v0/shader_list.txt.gz \
    --list config/US_v0/shader_list.txt.gz /wwhd/data/shaderlist/*.txt
tools/worker/sync.sh down config/US_v0/shader_list.txt.gz
# s-shadercover's captures (rendered, on a GPU: the desktop worker's did 379 rooms in ~20 min): the stage survey's
# chunks and routes, then the check of the merged list
python3 tools/stage_actors.py STAGE --spawns >> spawns.txt   # for every stage (and sea), on the worker
python3 tools/shaders/survey_route.py --list-rooms > rooms.txt
python3 tools/shaders/survey_route.py spawns*.txt --rooms rooms.txt --skip I_SubAN,Fairy05 --out chunks --chunks 8
tools/worker/job start cover bash -c 'tools/shaders/cover_runs.sh OUT 8 chunks/chunk0 ... chunks/chunk7 ROUTE...'
tools/worker/job start check bash -c 'LIST=merged.txt.gz tools/shaders/cover_runs.sh CHECK 8 chunks/chunk0 ... ROUTE...'
```

Every shader a capture records has been translated a second time from the registers its line keeps
alone, and must give the same key and the same record; one that doesn't is logged and left out.
