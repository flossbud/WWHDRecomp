# Steam Deck and Android: the performance plan

Session qa, 2026-10-08, for B75 (the Deck runs the game at 35-55% speed: CPU-bound). Built from
`docs/research/perf-baseline.md` (the baseline, the A/Bs, session bottom's half-frame split),
`docs/research/rival-study.md` (§6, their performance work) and `docs/research/texture-tracking.md`. Gains are per
60 fps frame on the 13700K (real time, headless, the save route `continue` unless noted) and an estimate for the
Deck's Zen 2 at ~2x per thread (its 2.4-3.5 GHz cores against the 13700K's P-cores: an estimate until the quick tests
below measure it).

## Where the time goes now (the 13700K, at 60)

| thread | continue | the Darknut fight (en-tn) | Deck estimate (fight) |
|---|---|---|---|
| game (scheduler) thread | 7.2 ms a frame (43% of a core) | 9.5 ms (57%) | ~19 ms: over the 16.7 ms budget |
| render thread | ~3.0 ms (after this week's two wins) | ~5.3 ms | ~11 ms, plus the GPU wait |
| the game thread waiting in GX2DrawDone (frame log `drawdone`) | 1.6 ms | 2.0 ms | grows with the render thread |

60 fps adds 73-83% to the game thread (bottom's split): the converted processes' half-step executes are only ~13%
of that, the actors' draws about as much (every actor draws every frame), and most is elsewhere: **the sound's AX
processing runs about five times its 30 fps cost**, the half frame's own path, the journal. On the Deck the game
thread alone doesn't fit 60 in a fight; at 30 it needs ~9 ms of a 33 ms frame. That is the plan's main fork.

## The quick tests (first, on the owner's Deck; an hour or two)

1. **Clocks and governor.** `cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor` and the frequencies while
   playing (`watch -n1 grep MHz /proc/cpuinfo`); SteamOS's power profile and TDP limit (Quick Access, Performance).
   On our own worker the `powersave` governor alone cost 10% at 30 (recompiler-design.md, "Idle threads and
   clocks"): a half-busy thread that sleeps a thousand times a second sits at low clocks. Retry with
   `performance` and the TDP at its maximum.
2. **30 fps** (`WWHD_60FPS` unset): if the game runs at full speed at 30, the Deck can ship now with 30 as its
   default and 60 as an option, while the items below bring 60 within reach.
3. **Per-thread CPU and a profile** on the Deck: the same three scenes (`perf-top.sh`, `WWHD_PROFILE`), so every
   estimate here becomes a measurement. The routes, the save and the scripts copy as they are (`~/wwhd-test`).
4. **Thread placement**: the game and render threads on different physical cores (not SMT siblings of one core),
   `taskset` to try; if it helps, the frontend sets the affinity.

## The items, in priority order

| # | item | gain, 13700K | gain, Deck (est.) | effort | risk | how it stays exact |
|---|---|---|---|---|---|---|
| 1 | **The sound at 60** (AX ~5x its 30 cost; session bottom is on it) | ~0.6 ms a frame on the game thread (8% of it) | ~1.2-1.5 ms | 0.5-1 day once the cause is found | low | the audio hash (`WWHD_AUDIO_HASH`) at 60 equals 30's; checks' sound |
| 2 | **GX2DrawDone waits for recording, not the GPU** (the rival's "lazy DrawDone", their biggest win: +13-53%) | up to 1.6 ms a frame of the game thread's wall time (a wait, not CPU) | more: the wait grows with a slower render thread | 1 day | medium (ordering: a CPU read of a GPU result after DrawDone) | checks' command streams and captures byte-identical; a route that reads back (picto box) |
| 3 | **Render thread: skip redundant work per draw** (the rival's "CPU paths": unchanged descriptors, vertex and uniform binds, a pipeline lookaside; `Descriptors`/`Textures` are 12-13% inclusive) | 0.3-0.6 ms (10-20% of the render thread) | 0.7-1.5 ms | 2-4 days, one A/B'd step at a time | low | each step: captures byte-identical, `perf-ab.sh` |
| 4 | **Half frames: static actors draw from their whole frame** (signs, bridges, palms, door knobs, the sky box: bottom's list) | ~0.2-0.3 ms (the actors' draws are ~6% of the game thread at 60) | ~0.5 ms | 2-3 days | medium (an actor taken for static that moves draws a frame late: the hz30 sweep catches it) | 30 unchanged (checks); the half frame's picture against the interpolation (hz30) |
| 5 | **The half step's journal** (`HalfTickStore` + `rt_journal_store`, ~3%) | ~0.2 ms | ~0.4 ms | 1-2 days | low | regress and predeploy unchanged |
| 6 | **Texture tracking by write-protection** (`texture-tracking.md`) | ~0.05 ms (the sampled check is ~1.3% of the render thread) | ~0.1-0.2 ms | ~2 days | medium (a writer the audit misses: the verify mode finds it) | the checks' verify mode, 0 missed writers; and it removes real time's up-to-a-second texture lag |
| 7 | **Link's half step cheaper** (his execute doubles at 60; the one actor worth it) | ~0.1 ms | ~0.2 ms | 1-2 days | medium (his half step's exactness work) | predeploy |

**Item 2, measured (session qa, WIP on ww-4-qa, not on ww-4):** the frame log's new `fence_ms` column (the render
thread's `vkWaitForFences` in `SubmitAndWait`, src/gpu/vk/renderer.cpp) against `drawdone_ms`, continue and house at
60: the game waits 1.58 ms a frame in GX2DrawDone, of which **1.45 ms is the render thread waiting for the GPU's
fence** at the present (`SubmitAndWait` at the flip; the timestamp the game waits for retires after it). So the lazy
DrawDone is worth up to ~1.5 ms a frame of the game thread's wall time here, more with the Deck's GPU. The change:
two of each per-frame resource (command buffer, fence, the upload ring's half, the descriptor pool); the present
submits without waiting and the next frame waits only on the fence of the frame whose resources it reuses; the
places that read the GPU's results into guest memory (the readbacks at draw.cpp's 1x1 copy, renderer.cpp's surface
and scan-buffer copies) and that destroy images (renderer.cpp's "then the old image can go") keep `SubmitAndWait`.
Behind a flag, off by default; the virtual clock (checks) keeps today's wait, so the checks stay exact by
construction; real time measured with perf-ab and the frame log (`drawdone_ms` should fall by about `fence_ms`).
About a day plus gates.
**Item 2, implemented as WIP (ww-4-qa, session qa):** `WWHD_LAZY_DRAWDONE=1` (renderer.cpp `SubmitFrame`,
`LazyDrawDone`; renderer_internal.h's slots). First real-time run, continue at 60 on the 13700K: the game's
GX2DrawDone wait 1.55 -> 0.11 ms a frame, the fence wait 1.44 -> 0, the render thread's CPU unchanged (2.88 / 2.87 ms).
`WWHD_LAZY_DRAWDONE=2` is a test that turns it on under the virtual clock too (the guest's time is its cycles there),
so the checks compare its pictures with the references. Left before it can be on by default: the checks with =2
(captures byte-identical), predeploy and an A/B with =1, an audit of `DestroyImage` callers that don't go through
`SubmitAndWait` (an image the other slot's frame may still use), and the windowed present (a semaphore from the
swap's submit to `PresentQueue`, then drop the `!HasWindow()`).
Checks with `WWHD_LAZY_DRAWDONE=2` (the lazy path on in the capture runs, its log line confirms it): traces, command
streams, sound and diff mode all MATCH, so the guest ran identically; **the captures are not byte-identical**: frames
360 and 420 of the 15 differ by 1-2 levels in 22 and 50 pixels (boxes 895-907 x 911-922 and 885-897 x 692-707, on
the title and menu screens; worst PSNR 98.15 dB), the other 13 exact. Renderer-internal, then: one small moving
element whose blend inputs differ. Suspects, in order: a texture whose guest memory the game writes while the render
thread uploads it (the fence wait used to hold the render thread back a frame's GPU time, so it now reads earlier: a
race that exists in real time either way, which the virtual clock's captures now expose); something carried across
the swap that assumes the GPU finished (a surface copy, `CopyOf`'s `written` check, the ring's slot halves). Next step:
`WWHD_RENDER_DUMP`/the draw stats on frame 420 with =0 and =2 to find the draw, then its texture's upload time. The
image-destruction audit found nothing unsafe (surface growth's destroy follows its `SubmitAndWait`, which drains both
slots; the overlay is windowed only; texture-cache images are never destroyed).
**Found and fixed:** the difference was the ring's halves, not the timing. `WWHD_LAZY_DRAWDONE=3` (two slots and the
halves, every frame still waited for) gave the same two frames; zeroing the 64 KB after each allocation in the normal
renderer (`WWHD_RING_POISON=1`) changed nothing. The halves cut a slot to 128 MB, so heavy title-screen frames hit the
ring-full submit mid-frame (draw.cpp's `s.ringEnd` check), which splits the frame and moves those pixels. With the
lazy path the ring is 512 MB (256 MB a slot, as without it): the captures byte-identical with =2. (So a frame that
fills the ring mid-way changes a few pixels in the normal renderer too: a latent exactness hazard past ~192 MB a frame.)
Cost: 256 MB more host-visible memory with the flag on.
**A/B** (perf-ab, the same binary with the flag on and off, alternating; game thread's work a frame at 60): continue
9.51 -> 8.09 ms (-1.42 ms, -15%), house 8.56 -> 7.14 ms (-1.42 ms, -17%); the DrawDone wait 1.58 -> 0.12 ms; the
render thread's CPU unchanged (3.12 / 3.14 ms, 3.61 / 3.65 ms); every run with it below every run without.
**With a window** (session qa): the swap's submit signals a semaphore per swapchain image and its present waits for
it (`PresentSemaphore`, present.cpp); a swapchain rebuild idles the queue and both slots before the old images go.
`WWHD_LAZY_DRAWDONE=1` now applies with a window too. Tested only on the worker: lavapipe on a private Xvfb, the
continue route at 60 to frame 2400 with it on and off, both clean, the window's picture right in both. Not yet:
the owner's GPU (a window on the desktop: the owner's to open), its A/B there (`drawdone_ms`, with vsync off and on),
and a run under the Vulkan validation layer (not installed on the worker).

**Item 2's present path against the Vulkan spec (session cloud2, a paper review, no runs; `PresentSemaphore`/
`PresentQueue`/`Acquire`/`Build` in present.cpp, `SubmitFrame`/`SubmitAndWait`/`BeginSlot`/`Submit` in renderer.cpp,
`DrawOverlay`).** Right by the spec:
- *Binary semaphores.* The signal is submitted before the present that waits for it (`RendererSwap`: `PresentRecord`,
  `SubmitFrame`, `PresentQueue`), and each signal gets exactly one wait (`PresentSemaphore` only on the lazy submit,
  `PresentQueue` always right after; on `OUT_OF_DATE`/`SUBOPTIMAL` the present's wait still executes, the spec says
  so). "Unsignaled when signalled" (VUID-vkQueueSubmit-pSignalSemaphores-00067) holds because there is one semaphore
  per swapchain image, signalled only after that image was acquired again with its fence waited, which implies its
  last present's wait ran: the pattern the Vulkan guide's "swapchain semaphore reuse" page recommends (a semaphore per
  frame in flight would be the bug).
- *Fences.* Every submit's fence is unsignaled and not in use (`WaitFence` resets right after the wait; a slot records
  only after its fence; `SubmitAndWait` waits both slots). The acquire's fence is waited and reset every swap.
- *Reuse.* A slot's command buffer and descriptor pool are reset only after its fence (`BeginSlot`); the set cache is
  cleared there, so no set crosses slots; the ring's halves are per slot; the index cache copies into a fresh
  `RingAlloc`. A ring-full `SubmitAndWait` inside `PresentRecord` (the overlay's upload) is fine: the swap's signal
  covers everything earlier in submission order.
- *Presents without a semaphore* (lazy off, `PresentOverlayOnly`) follow a host fence wait: the writes are complete
  and available, so the presentation engine sees them. Pre-existing and fine in practice (the spec's prose asks for
  semaphores; no VUID, validation doesn't flag it). Submit and present use one queue from one thread
  (`PresentOverlayOnly` runs before the GPU thread starts).

Problems, in order:
1. **The overlay image is destroyed while the other slot may still read it** (VUID-vkDestroyImage-image-01000,
   vkDestroyImageView-01026). `DrawOverlay` calls `DestroyImage` when the overlay's size or format changes; the last
   frame's submit (the other slot, not waited for) may still be blitting from it. The image-destruction audit above
   predates the windowed lazy path ("the overlay is windowed only"). Core validation flags it whenever the overlay
   resizes during play. Fix: `WaitPending()` before that `DestroyImage` (rare: a resize of the overlay).
2. **Frames now overlap on the GPU without barriers between them.** The fence wait at each swap used to separate
   frame N's GPU work from N+1's; now N+1's commands may run while N's do. Barriers are recorded only on layout
   changes (`Transition`, `BeginRendering`), so an image written at the end of one frame and again at the start of the
   next in the same layout (a render target that stays `COLOR_ATTACHMENT_OPTIMAL`, a depth buffer) is a write-after-
   write hazard with no barrier. Two render passes on one target within a frame have the same gap already; the frame
   boundary used to hide it across frames. Runs look clean because drivers tend to serialise submissions, which the
   spec doesn't promise. Fix: one global memory barrier at the start of each command buffer in `BeginSlot` when the
   lazy path is on (`ALL_COMMANDS` to `ALL_COMMANDS`, `MEMORY_WRITE` to `MEMORY_READ | MEMORY_WRITE`). N+1's GPU work
   then starts after N's, as before; the CPU gain (the point of the lazy path) stays, and the captures can't move.
   The validation run should enable synchronization validation (`VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT`)
   to show this, and probably the within-frame cases too.
3. **Minor: a swapchain rebuild destroys the per-image semaphores after `vkQueueWaitIdle`.** A present takes no
   fence, so formally nothing proves its semaphore wait has run (the guide's point: only
   `VK_EXT_swapchain_maintenance1`'s present fences do). Reusing them for the new swapchain wouldn't be provably safe
   either. Every engine does the idle wait and the layers accept it, so leave it, or later use the extension where
   present (recent Mesa has it; check the Deck's driver).
4. **For the vsync A/B (not a spec issue):** `Acquire` waits for its fence on the render thread before the swap's
   submit. With FIFO (`WWHD_VSYNC=1`) that blocks the render thread at the display's rate, and the game's GX2DrawDone
   waits on the render thread's recording, so with vsync on the gain may show as steadier pacing rather than a lower
   `drawdone_ms`. One acquired image at a time keeps VUID-vkAcquireNextImageKHR-swapchain-01802.
5. Aside: the scan buffer's re-creation (`dst = CreateImage(...)` in the copy to the scan buffer) drops the old image
   without destroying it. That leaks on a size or format change, and isn't a hazard. If it's fixed, it needs
   `WaitPending()` first, as in 1.

Fixes 1 and 2 are a few lines each; neither can change a capture (the virtual clock never takes the lazy path,
and =2's captures would show it if it did).

Done this week (`perf-baseline.md`): ProgramHash's copy check (render thread -7 to -9%), the index cache (bottom,
~-0.3 ms on continue), the sampler key (SampleTexture 7.2% -> 5.6%).

Items 1-5 together are worth ~2.5-3 ms a frame on the game thread's wall time on the 13700K, ~5-6 ms on the Deck:
enough for 60 on the save route, likely not in the heaviest fights (~19 ms estimated). So:

- **Ship 30 on the Deck first** if quick test 2 shows full speed, with 60 as an option;
- **60 on the Deck in fights** needs more than these items: either a deeper cut of the half frame (only the converted
  processes and the draw run again, not the frame's whole body: a design of its own, the largest lever) or a
  30 Hz game with interpolated presentation for weak CPUs. Decide after the quick tests' numbers.

**Item 3, a first step (WIP on ww-4-top, session top, parked at the 95% wrap-up):** the draw path no longer sets an
unchanged pipeline, viewport, scissor, blend constants or depth bias again within a command buffer
(renderer_internal.h's `Bound`, draw.cpp; cleared at each vkBeginCommandBuffer; every pipeline has those four
states dynamic, draw.cpp's one pipeline creation, so they outlive a pipeline change). `WWHD_BINDCACHE=0` turns it
off. Exact: route tour3's captures at 13 frames (f950-2100) byte-identical with it on and off. Not yet measured
(`perf-ab.sh` on the desktop, real time) and not on ww-4. Left for item 3: the vertex buffers and descriptor sets
change every draw (fresh ring offsets and sets), so the bigger part is `Descriptors`/`Textures` themselves (a set
reused when its images and uniform data equal the last one's) and a pipeline lookaside before `GetPipeline`.

**Item 3, the next steps' design (session top):** two kinds of per-draw work repeat what the last draw did.
- *Descriptor sets.* `Descriptors` (draw.cpp) allocates and writes a set per stage per draw, but its contents
  depend only on the shader's layout (`sh.layout`), the uniform vars' range (`offset_endOfBlock`) and the images
  (`Textures`: sampler, view, layout per binding): every buffer descriptor points at the ring at offset 0 and the
  draw's place in the ring goes in the dynamic offsets. So a set can be reused while the descriptor pool lives (it
  is reset at every submit, `BeginSlot`/`SubmitAndWait`): a map from (layout, vars range, the images' handles) to
  the set, cleared with the pool; a hit skips `vkAllocateDescriptorSets` and `vkUpdateDescriptorSets` (and its
  counts for `Reserve`). Exact: the set's contents are the same handles. Watch: an image view or sampler destroyed
  and its handle reused within a submit (the map must be cleared where views or samplers are destroyed, or keyed on
  their creation number).
- *Uniform blocks.* `UniformBlock` takes 64 KB of the ring per block per draw, clears it and copies the block's
  guest bytes (up to the shader's size). When a block's source (address, size) and bytes equal the last copy's in
  this submit (a memcmp against a host copy, as the index cache does), the last ring offset can be returned: the
  draw reads the same bytes at the same place. Exact by construction; it also takes far less of the ring, so fewer
  mid-frame `SubmitAndWait`s. `UniformVars` (the registers' constants, small) the same way.
- *A pipeline lookaside.* Before `GetPipeline`'s full key, the last draw's key and pipeline (most draws repeat the
  state of the one before).
Each step: `WWHD_RENDER_STATS` counts of hits, captures byte-identical on and off, `perf-ab.sh` (render thread).

**Item 3, two more steps, on ww-4 (session cloud, measured on the worker):**
- *Uniform blocks filled only as far as the shader reads.* `UniformBlock` took 64 KB of the ring per block per draw,
  zeroed it all and copied the block. The shader declares the block as its quick buffer's size (Cemu's
  `DetermineSize`: the highest static index + 1, or the whole 64 KB with a dynamic index) and reads nothing past it,
  so now only that much is taken and filled (the block's bytes, then zeros); the descriptor's 64 KB range stays in the
  buffer (`RingAlloc` keeps 64 KB spare). `WWHD_UBLOCK_FULL=1`: as before.
- *Descriptor sets reused* (the design above): a set's contents are the shader's and the images' alone, so a draw
  whose shader and images (sampler, view, layout) equal an earlier one's since the pool's reset binds that set (no
  allocate, no update); cleared with the pool and whenever an image view is destroyed. 92% of sets reused on continue
  (`WWHD_RENDER_STATS`). `WWHD_SETCACHE=0`: off.
- A/B (`worker-ab.sh`, continue at 60, lazy DrawDone on in all, 5 rounds paired): the render thread's CPU a frame
  9.94 -> 9.12 ms with the first (-8.5%), -> 8.69 ms with both (**-12.9%**, every run below every run); the game
  thread 19.35 -> 19.10 ms (-1.3%, lower in 5/5 rounds: the power cap's share); 49.3 -> 49.9 fps.
- Gates: checks all MATCH, captures PSNR inf; regress identical to the last ww-4 run; predeploy 50 ok, gohmatail WARN
  54.0 and gohmarock 4.8 as before.
- Left: the render thread's libc time on the worker (29% of it; ~16% is `RendererDraw`'s vertex data copied into the
  ring per draw, ~4% the index cache's compare and copy) and the driver (23%). A vertex copy can't be skipped without
  knowing the guest's bytes are unchanged, and a compare costs about what the copy does: not worth it without write
  tracking (item 6). A pipeline lookaside isn't worth it either (`GetPipeline` 1.4%).

**Item 4, designed, built and measured: no gain, parked (session cloud3, WIP on branch ww-4-cloud3, not on ww-4).**
- *Why a static actor's whole-frame draw can't simply be replayed.* Every process draws on every frame (the game
  rebuilds its draw lists each frame: `f_025DE2CC`'s note), and most of a model's draw depends on the camera, which
  moves on half frames: fopAc_Draw's cull check (`f_025D6CE8`), the entry into the draw lists (sorted by view
  depth) and J3DModel::viewCalc (`f_027F55FC`, via `f_025E2BF4`, the HD port's mDoExt_modelUpdateDL tail). In this
  port viewCalc fills, per model and per call, two 0x23C-byte blocks with the view matrix and the eight lights in
  view space (`f_027F53CC`/`f_027FDA54`, from j3dSys's view at 0x104B45F8; the model's +0x70 is 2 for every model,
  +0x60 the blocks) and moves a double buffer on (`f_027FB678`). So a replay would have to redo all of that anyway;
  a recorded call list would also have to carry the context draws set around their models (j3dSys's draw buffers,
  HD globals such as the palms' 0x104B4634/38, material animations entered on shared model data).
- *What can be reused:* J3DModel::calc (`f_027F4D5C`: joints' world matrices, the material calc), from the model's
  base scale and matrix (+0xBC to +0xF8), its animations and its joint callbacks: camera-free. Built:
  `WWHD_60FPS_STATICDRAW=1` skips a half tick's calc of a model calculated once in the same process's whole-tick
  draw when its base is unchanged, Link doesn't hold it and no store since the half tick began changed a byte of
  its process or heap (the actor's heap is +0xF4, a JKRSolidHeap whose block is [heap, heap+0x5C); the stores come
  from the half tick's page table, with a watch bit; the draw lists' links, `LiveStore`, aside). `=2` runs each
  calc it would skip and compares what it changed (the heap whole: the calc first clears what it writes with an
  imported memset the store hook doesn't see).
- *Correctness:* tour3, 12,379 would-be skips checked: all exact but type 407's (unnamed, drawn like Obj_Lpalm):
  its joints sway from something outside its own state. (Before the value compare, every converted actor counted
  as changed: a half step stores its state every frame, mostly the same values.)
- *Gain:* continue at 60 (desktop, real time, WWHD_PROFILE, one run each): 22% of the half ticks' calcs in draws
  skipped (21,756 of 100,231: most actors change every half tick), J3DModel::calc 725 -> 709 samples of ~22,300 on
  the game thread (noise), the watch +42. The static models are the cheap ones (few joints). The other
  camera-free parts of a static actor's draw (settingTevStruct `f_025626A4`, setLightTevColorType `f_02562F5C`:
  ~0.9% of the game thread at 60 for every actor's half-frame draw together) would gain less than the watch costs.
- *A lead outside item 4, checked and dropped:* viewCalc fills both of a model's blocks with the same view
  (`f_027F53CC`, 2.4% of the game thread at 60 on continue). The GamePad's screen shows only the item menu, and
  the TV reads the second block too: with it left unfilled (a probe on `f_027FDA54`) 5 of 7 tour3 captures at 30
  changed.

**Item 5, landed (session cloud3, ww-4 938730f):** the journal's remaining cost was mostly a fast path's quiet
watch: a half tick's frame runs inside a watched task call (task_loop.cpp), and `g_rtStoreAll` stayed set after
the watch had already failed, so every store of the frame (93k a half frame on continue, the heap included) went
through `rt_journal_store` and `QuietStore`. Now the watch drops it once the call is visible, and
`rt_journal_store` returns first for bytes saved already or not kept (handoff.md, "Session cloud3"). Continue at 60,
desktop profile: the journal 2.1% -> 1.1% of all samples, its calls 93k -> 33k a half frame. `WWHD_JOURNAL_SKIP=0`
for an A/B. **A/B on the worker** (worker-ab.sh, continue:1800, 6 rounds paired, lazy DrawDone on in both): the game
thread's work a frame 20.05 -> 19.67 ms (-2.1%, lower in 6/6), 49.9 -> 50.8 fps, every run better. Left: the 33k calls' own cost (~1%): a check inline in `RT_STORE` (ppc_ops.h; it grows every store site).

## Android

The same items apply (the code is C++; the recompiled program builds for ARM64 like the rest), plus what the rival
needed on phones (rival-study §6): the two heavy threads pinned to the fastest cores (`/sys/.../cpuinfo_max_freq`),
an ADPF hint session (`APerformanceHint`) for the game and render threads' target durations, and a thermal-aware
frame rate (`AThermal_getThermalHeadroom`: 60 while there's headroom, 30 when hot). The rival reached 30-32 fps in
the heaviest scenes on a Galaxy S25 Ultra. An Android build of ours doesn't exist yet: the port itself (Cemu's
platform layer, a Vulkan surface, input) comes before any of this and is the larger effort (weeks, not days).

## Gates for every item

Checks (traces, command streams, sound, captures byte-identical, PSNR inf), regress, predeploy 0 FAIL, and an A/B
(`~/wwhd-test/perf-ab.sh`, alternating builds, render or game thread per frame from the frame log) for the claimed
gain. Profile shares (`WWHD_PROFILE`, `tools/profile_report.py`) when the A/B can't resolve a small change. Parallel
runs need a binary each.
