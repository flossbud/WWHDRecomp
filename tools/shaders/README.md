# tools/shaders - the shader corpus (G1)

Every GX2 shader program the game can bind, translated ahead of time to SPIR-V
(docs/recompiler-design.md D14). Everything these tools write is derived from game data: it goes
to `build/` or the worker's `/wwhd/data`, never into git.

| File | What |
|---|---|
| `corpus.py` | walks the title's `content/` (Yaz0, SARC, SHARCFB v9, GFD `.gsh`, and GFD files embedded in BFRES), writes each distinct program once (`programs/<hash>.<vs\|ps>`: its GX2 registers and microcode) and `index.csv`; checks the programs a run used are all there |
| `translate.cpp` | Cemu's shader decompiler plus glslang, as Cemu's Vulkan renderer runs them, but offline: GLSL and SPIR-V for every program |
| `build.sh` | builds `build/shaders/translate` against the worker's Cemu (Cemu_release's link line with `--wrap=main`, like the M1 fuzzer) |

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
