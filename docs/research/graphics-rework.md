# Graphics rework (g-rework): research

The owner's ask (2026-10-14): the game as good as the original or better, running better on low-end and high-end
devices alike, never a look traded for speed; not tweaks but whole systems (a native renderer of the game's scenes,
our own shaders, batching and instancing, the same effects at a lower cost, render paths for tile-based mobile
GPUs), each step measured on the Odin and the Deck class and compared with the original's picture. This is the
research the plan rests on: what the renderer does now, where the time goes, what the frame is made of, and what
each idea is worth. Session bottom, from 2026-10-15.

## 1. The renderer today

The front half is exact by construction: the game's GX2 calls go through our port of Cemu's gx2 into PM4 command
buffers byte for byte as Cemu's (the game reads display-list sizes; recompiler-design.md D12), and the command
processor (`src/gpu/null_gpu.cpp`) keeps Latte's register file. The back half is ours: at each draw `draw.cpp`
translates what the registers say into Vulkan, draw by draw.

- **Shaders**: the program's microcode goes through Cemu's decompiler to GLSL, glslang to SPIR-V (D15, D20). The
  shipped shader list (1,382 shaders, 1,000 pipelines) makes them before the first frame.
- **Draw data**: uniforms, vertices and indices copied from guest memory into a per-frame ring at each draw; a
  descriptor set per stage (uniform buffers dynamic: the data's place goes in the offsets, so a set depends only on
  the shader and its images).
- **Surfaces**: each guest colour or depth buffer is a VkImage keyed by address and format; a texture at a surface's
  address samples the surface (a copy when the size or format differs, or when the draw samples its own target);
  other textures are decoded from guest memory and re-hashed for changes.
- **Passes**: dynamic rendering, one pass per run of draws into the same targets (`BeginRendering`): every
  attachment loaded (`LOAD`) and stored (`STORE`), a pending whole-image clear done as the load op. On the RP4's
  Mali (Vulkan 1.1) `passes.cpp` turns each into a one-subpass render pass.
- **Present**: the scan buffer drawn into the window, the extras (FSR 1 and 3, FXAA, frame generation, motion
  vectors) each a pass of its own, all opt-in.

A frame of sail at 60 (the worker, the virtual clock, frames 2400-2700):

| | a frame |
|---|---|
| draws | 1,308 |
| render passes | 48 (only 2 resumed on the same targets) |
| uniform vars | 863 KB (32% a repeat of an earlier upload) |
| uniform blocks | 20 KB + 316 KB zeros (after a4-gpu's reuse) |
| indices | 973 KB from the decode cache (72% a repeat) |
| vertices | 6.1 MB (27% unchanged from an earlier frame) |
| descriptor sets written | ~510 before; ~0 with the kept pool (setcache, 2026-10-15) |
| consecutive draws with the same descriptor sets | 0% |

The targets, by what draws into them: the scene at 1920x1080 (A2B10G10R10, R8, D32F; a 1920x1088 twin of the
colour); a 960x544 chain (RGBA8, its own D32F, R16F, RGB10A2) with the ambient occlusion; bloom mips in B10G11R11
from 480x272 down; the shadow map, 1024x1024 D16 in three layers; small 512x512 and 256x256 targets; the GamePad's
864x480 (about 7 draws).

The game's own renderer is barely mapped: of the 194 names in `config/US_v0/symbols.csv`, the renderer has
`RenderDisplay_draw` and `RenderDisplay_calcGPU` (the HD engine's render job lists), `gfx_EndFrame` and `fw_procDraw`
(the TV's and the GamePad's draw passes). Its models, materials and post chain have no names yet.

## 2. Where the time goes

**CPU, the GPU thread.** The desktop: ~2.8 us a draw (the rival: ~1.7). The Odin (top's profile, sail at 60, the
system driver): ours 36%, the driver 33%, libc 26%; vkCmdBeginRendering 13.6% (Qualcomm's driver allocates GPU
memory at each begin), descriptors 10.5% (vkAllocateDescriptorSets 3.5, vkUpdateDescriptorSets 2.0, ours 5.1),
BeginSlot 8.6% (vkResetCommandBuffer), SampleTexture 4.9%, ProgramHash 4.8%. Turnip took ~33% fewer of the
thread's cycles than the system driver. The descriptor share is what the kept pool removes (its Odin A/B waits for a
window).

**GPU.** The worker's iGPU (the Deck stand-in): 18.7 ms a frame before the exact wins, bandwidth-bound (~28 GB/s of
memory moves); by kind passes 11.4 ms, our size copies 4.5, clears 1.0, the scan copy 0.6, mips 0.6 (gpu-plan.md).
The RP4 (Mali-G77) in sail at 60: 52-55 fps with the automatic scale, the GPU 14.1 ms a frame at render scale 0.5,
~15.5 at 0.625 and ~17 at 0.75 (main's runs). Its timing by pass (WWHD_GPU_TIMING, sail at 60, 2026-10-15; the timing
itself costs Mali ~10 fps, so these are the split, not the speed):

| | scale 1 (1334x750) | scale 0.5 (667x375) |
|---|---|---|
| the GPU, a frame | 18.9 ms | 14.4 ms |
| passes | 14.8 | 10.7 |
| the scene's passes (scaled) | ~6.8 | ~3.6 |
| 960x544 RGBA8, 3.9 passes (AO off's variant, the shadow pass, two blurs) | 2.4 | 2.4 |
| mips 480x270 B10G11R11, 9 a frame | 0.7 | 0.7 |
| the shadow map (1024x1024 D16, 3 layers), 2.3 passes | 0.6 | 0.5 |
| its depth clears, 2.9 a frame (not load ops: layered) | 0.4 | 0.5 |
| copies | 1.2 | 1.0 |
| 960x544 two targets + 960x544 RGB10A2 | 1.0 | 1.0 |
| present + scan | 0.6 | 0.5 |

So ~10.8 ms of the RP4's 14.4 doesn't scale: the half-size effect chain, the shadows, the mips, the copies.

**Attachment traffic** (a probe, this research; sail and tour3 at 60 on the worker, a frame's average over frames
2400-2700). Each pass loads its attachments and stores them at its end; a tiler (Mali always, Adreno in its tiled
mode) moves exactly these bytes between tile memory and DRAM:

| | sail | tour3 |
|---|---|---|
| passes | 48 | 29 |
| attachments loaded | 232 MB | 45 MB |
| cleared (load op) | 34 MB | 14 MB |
| stored | 266 MB | 59 MB |
| of the stores: read after | 254 MB | 49 MB |
| of the stores: dead (cleared or destroyed unread) | 12 MB | 10 MB |
| loaded again by the very next pass | 38 MB | 0 |

At 60 fps sail's 500 MB a frame is ~30 GB/s: about all of the RP4's memory bandwidth (the Dimensity 1100's LPDDR4X)
and half of the Odin's. These are uncompressed bytes of the whole render area, so an upper bound: framebuffer
compression (Mali's AFBC, Adreno's UBWC) shrinks them, and Adreno may render a pass straight to memory instead (its
direct mode: no load or store, but every blend and depth test then reads DRAM). The device timing says which. By
attachment (sail):

| attachment | loaded | stored | passes using it |
|---|---|---|---|
| 1920x1080 D32F (the scene's depth) | 87 MB | 95 MB | 12 |
| 1920x1080 A2B10G10R10 (the scene's colour) | 79 MB | 95 MB | 12 |
| 1920x1088 A2B10G10R10 | 32 MB | 32 MB | 4 |
| 1920x1080 R8 | 14 MB | 14 MB | 7 |
| 960x544 RGBA8 | 8 MB | 8 MB | 4 |
| 1024x1024 D16, 3 layers (shadows) | 6 MB | 6 MB | 3 |
| 960x544 D32F | 0 | 4 MB (never read) | 2 |

**Why the passes end** (sail, a frame): 31 by texture work in the middle of rendering (a copy of a surface 12, mips
for a chain 14, a surface moved to a sampled layout 5), 12 by a change of targets, 5 by clears and the scan copy.
Of the 31 texture breaks, 24 are for an attachment of the pass itself (the draw samples what the pass drew: a
feedback copy, or the next level of a downsample chain), 7 for another surface. And how each pass's targets relate
to the pass before it:

| the pass before ended by | same | fewer (a subset) | more (a superset) | overlapping | unrelated |
|---|---|---|---|---|---|
| a change of targets | 0 | 4 (34 MB loaded) | 3 (51 MB) | 3 (42 MB) | 2 (8 MB) |
| texture work | 1 (2 MB) | 1 (16 MB) | 0 | 1 (10 MB) | 28 (63 MB) |
| other | 1 | 0 | 0 | 0 | 4 (7 MB) |

So most passes are the game's own structure (28 a frame start on targets unrelated to the pass before), but ten
boundaries a frame fall between passes that share attachments, and they carry 127 MB of the 232 MB loaded.

**Fewer**: the next draw uses some of the open pass's targets and not others, typically depth left out because the
draw doesn't test it (Cemu's `GetActiveDepthBufferMask` detaches depth from a draw that neither tests nor writes it,
and a new pass begins). A draw into a subset of the open pass's targets can draw in that pass with its pipeline made
for the pass's targets and the other targets' writes masked off: the same pixels. Sticky passes (2026-10-15, on by
default, `WWHD_STICKY_PASS=0` off): sail 48 -> 43 passes a frame, loaded 232 -> 171 MB, stored 266 -> 204 MB (-26%
each way); the checks' captures identical. On the RP4 (sail, scale 1, A B A B, fps without timing, the 60 part):
42.5 / 44.0 fps off, 45.4 / 45.1 on (+4.6%). Its GPU time moved less (18.86 -> 18.68 ms with timing on), so part of
the gain is the GPU thread's: five render-pass begins a frame fewer.

## 3. The ideas, weighed

**A native renderer of the game's scenes.** Drawing the scene from the game's own structures (models, materials,
lights, the post chain) instead of its GX2 commands would free us from the command stream's shape entirely, but it
means mapping the HD engine's renderer first (its render job lists, model and material formats, every effect), and
its pictures would leave the checks' byte-exact comparison against the reference for good. The rival recomp didn't do
it either (its renderer translates draws like ours). The parts of it that pay off can come without it: the frame
graph below gives the pass structure, and our own shader translation gives control over the shaders. Revisit once
those exist; not the first step.

**A frame graph: record the frame, plan the next.** The game's frame is the same from one frame to the next (the
same passes in the same order, the same targets). Recording it (the probe above: each pass's targets, load and store,
what reads each store and when, why each pass ended) and planning the next frame from it allows what one draw's view
can't: begin a pass with the attachment a later pass of the run will need (the 3 "more" boundaries: 51 MB loaded),
store `DONT_CARE` where the recorded store was dead (12 MB on sail: the 960x544 depth, never read, and the scene's
depth after its last use), hoist copies of other surfaces out of a pass (7 breaks a frame), and on render-pass
devices put overlapping passes into one render pass as subpasses (3 boundaries, 42 MB). Each choice holds only while
the frame matches the recording; a mismatch falls back to load and store for that pass. Checked by a mode that turns
every `DONT_CARE` into a clear to a loud colour (the checks' captures then show any pixel read from a dropped store)
and by the captures with the plan on.

**Render paths for tile-based GPUs.** The traffic above is the tiler's cost; the frame graph's cuts are the tiler
path. Beyond them: input attachments for passes that read the pixel they draw (a light or composite pass reading
the G-buffer at its own position: the shader's texture coordinate is the fragment's own), which keeps the G-buffer in
tile memory (needs our own shader translation to rewrite the read); transient attachments (lazily allocated) for
targets that never leave the tile (the 960x544 depth). Measure first on the device: per-pass GPU time on the RP4 and
the Odin (WWHD_GPU_TIMING; it costs ~10 fps on Mali, so fps come from runs without it), and the render-scale sweep
that splits the fixed 11-12 ms from the scaled part.

**Our own shaders.** The decompiler's GLSL emulates the register file (every value a bit-cast integer and float,
exact-multiply emulation): the earlier test (gpu-plan.md item 5) found SPIRV-Tools' optimiser +1.9% and an exact
branch-free multiply +0.5% on the iGPU, which is bandwidth-bound; phones may be ALU-bound and are where to measure
again. A translator of our own (also what m-nocemu needs in place of Cemu's decompiler) that types values, folds
constants and drops dead code would give mobile compilers cleaner SPIR-V, and is what input attachments and any
per-pass replacement need. Checked by the captures (byte-identical where exact).

**Batching and instancing.** 1,308 draws a frame on sail, and no two consecutive draws share their descriptor sets
(each draw's images differ). Batching would need the shaders to fetch per-draw data by an index (multi-draw
indirect, bindless textures): again a job for our own translator. The CPU side's cheaper wins come first: the kept
descriptor pool (done), a vertex buffer cache (6.1 MB a frame copied, 27% unchanged; the rival's guest buffer cache
cut uploads 43-64%), the render-pass begin cost on Qualcomm (passes.cpp's render passes against dynamic rendering,
an Odin A/B pending).

**The same effects at a lower cost.** The ambient occlusion first. Android ships with `WWHD_AO=0` (the Performance
preset, which Auto picks on a low-powered device), which only makes the AO pass write 1: the pass and both blurs
still run, because the shadow pass fills the same half-size target's other channels (gpu-plan.md, "Graphics
options"), so "off" saves little and loses the original's look. On the RP4, with AO off, that 960x544 chain (the AO
pass's variant, the shadow pass, two blurs) is 2.4 ms a frame at any render scale, the biggest single cost that
doesn't scale. AO on adds ~1 ms to it: the chain 2.39 -> 3.36 ms, the GPU 18.72 -> 19.63 ms a frame (+4.9%; RP4,
sail, scale 1, timing on). The goal is not a cheaper "off" but AO cheap enough to turn back on by default on low-end
devices, the original's look restored: that ~1 ms is the number to beat. Then the bloom and downsample chains (9 mip levels
built a frame, 0.7 ms) and the shadow map (its three layers, 0.6 ms, and their separate clears, 0.4 ms, which as load
ops would cost nothing on a tiler: clears are load ops only for single-layer images today). Each replacement must
match the original's picture (the captures, PSNR against the reference) or exceed it.

**Better looks.** Render scale above 1, shadow resolution, anisotropic filtering, FXAA (all exist); temporal
anti-aliasing on the motion vectors; the frame graph and our own shaders make room for them on the devices that can
afford it.

## 4. The plan

1. **Measure on the devices**: the RP4's sail is above; the Odin (Adreno: tiled or direct by pass) and tour3/en-tn
   next, fps without timing; the Deck class on the worker's iGPU.
2. **Sticky passes**: done (on by default, its pipeline variants in the shipped shader list). The Odin's A/B when the
   owner frees it (BeginRendering is 13.6% of its GPU thread).
3. **Layered clears as load ops**: done (the shadow map's per-layer clears by the next pass into each layer; exact).
   On the RP4 neutral: the separate clears 0.43 -> 0.15 ms a frame, the shadow pass 0.67 -> 0.95, the GPU 18.64 ->
   18.72 ms (Mali's clear cost moved into the pass). Adreno, where a clear outside a pass is a write of its own to
   memory, is the device to judge it on.
4. **AO cheap enough to be on by default** on low-end devices (the original's look back), then the mip chains.
5. **The frame graph**: the probe as a recorder, then the planner (superset begins, dead stores, hoisted copies, the
   loud-clear check), measured on the RP4 and the Odin.
6. **Our own shader translator** (with m-nocemu), then input attachments and transient attachments on the tile path.
7. A native scene renderer reconsidered with 5 and 6 in hand.

Every step: the checks' captures identical (or compared by PSNR where a replacement is meant to differ), measured on
the RP4, the Odin and the worker, the look kept or bettered.
