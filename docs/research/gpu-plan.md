# The GPU side: 60 on weak GPUs, and a settings menu (the owner's plan, 2026-10-08)

Why: on the worker (the Deck stand-in) the CPU side of 60 now fits (three host threads and the lazy DrawDone: the game
thread 15.3 ms a frame on continue; docs/research/threads.md), and the frame rate stops at ~54 fps because its Intel iGPU
needs ~18.7 ms a frame for the game's rendering (the render thread waits on its fences). 60 needs ~11% off the GPU.
Weaker hardware (phones, the Deck in heavy scenes) needs more. The owner's order:

1. **Measure where the GPU's time goes.**
   - Per render pass, with Vulkan timestamp queries (`vkCmdWriteTimestamp` around each `BeginRendering`/
     `EndRendering` pair, each copy and each upload), summed per frame by kind: shadow maps, the main scene, the
     post effects, the game's copies (GX2CopySurface, the scan-buffer copy), our own copies and uploads, clears.
     A switch (`WWHD_GPU_TIMING=1`), real time, a line every N frames and a summary at exit.
   - The GPU's clock under the worker's 45 W package cap (`/sys/class/drm/card*/gt_act_freq_mhz` on the host, sampled
     during a run): CPU and GPU share the cap, so a busier CPU (three host threads) may slow the GPU.
2. **Exact wins** (the checks' captures stay byte-identical; each A/B'd on the worker):
   - barriers: today every image transition waits for all earlier work ("correctness first", renderer.cpp
     `Transition`): narrow them to the stages and accesses involved, so the GPU overlaps passes;
   - passes our renderer adds: surface copies and `CopyOf`, the scan-buffer copy, full loads or clears of targets the
     game overwrites anyway (`loadOp`), redundant layout transitions;
   - texture re-uploads of unchanged data (session cloud2's write-watch, texture-tracking.md);
   - whatever the measurements show next.
3. **If the exact wins fall short: render scale**, and the owner wants it anyway for weaker hardware: the game's
   render targets at a scale (0.5-1x, later above 1) with the guest's sizes kept for aliasing and copies (rival-study
   §4.8: theirs is 1-3x), then dynamic resolution (lower the scale only while frames run over budget). Off (1x) by
   default and in every check.
4. **A settings menu** (the frontend; the debug menu's machinery, `src/os/debug_menu.*`, `src/frontend/overlay.cpp`):
   render scale, display resolution and window mode, vsync, the frame rate (30/60), three host threads, the lazy
   DrawDone, ambient occlusion on/off (the rival fixed and gates theirs: rival-study §0), and later the shader set
   (item 5). Saved to `portable/settings` alongside Cemu's, applied at start or live where safe.
5. **Shaders optimised offline.** Today's shaders are Cemu's decompiler's translation (GLSL, exact-multiply emulation,
   integer/float conversions). An optimised set, prepared ahead of time (D20's shader list), selectable in the
   settings menu next to Cemu's translation (kept as the reference and the fallback), so the two can be compared and
   the optimised one switched off if it misbehaves. Checks run Cemu's set; the optimised set gets its own capture
   comparison (within tolerance where it isn't bit-exact) and A/Bs.

**Status (2026-10-09; queue ids in brackets):**
- Item 1: **done**: measured on the worker too (`b-gpumeasure`): its iGPU 18.7 ms a frame, 13.5x the desktop's
  1.38, bandwidth-bound (below).
- Item 2: **narrow barriers and surface fit on by default** (`b-barriers`, below): the worker holds 60.0 fps at full
  speed on the continue route (52.4 before). Left: load ops and redundant transitions (`b-clears`).
- Item 2's rest: **clears as load ops on by default** (`b-clears`, below; -1.9% GPU on continue). Redundant
  transitions: not worth doing (barriers and gaps 0.05 ms a frame on the worker).
- Item 3: **fixed scales and dynamic resolution built, off by default** (`WWHD_RENDER_SCALE=0.75|0.5|auto`; Auto in the
  Performance preset, the owner's choice of 2026-10-09). The worker on en-tn at 60: 100% 49.9 fps, 75% 56.6, 50% 59.7, Auto 59.2 (`b-scale`, below).
- Item 4: **first round and presets landed** (below). Left: the AO toggle, AF, FXAA (`b-gfxopts`).
- Item 5: not started (`b-shaders`).

## Item 1 on the worker: where its iGPU's 18.7 ms go (session bottom, 2026-10-09)

`worker-ab.sh continue:1800 3`, main 2d10fe1, the lazy DrawDone in every variant, `PERF_CLOCK=1` (the iGPU's
clock, `gt_act_freq_mhz`, every 0.5 s: readable inside the container) and `PERF_KEEPLOG=1` (the timing lines;
`worker_absum.py` sums the ones in gameplay, presented frame 1800 on at 60). Medians of 3 rounds after a warm-up:

| variant | fps | game speed | GPU ms a frame (timing) | GPU clock | game thread CPU a frame | render thread fence wait |
|---|---|---|---|---|---|---|
| three host threads, timing on | 53.1 | 88% | **18.70** | 1149 MHz | 15.1 ms | 4.1 ms |
| one host thread, timing on | 48.2 | 80% | 18.33 | 1142 MHz | 20.1 ms | 0.75 ms |
| three host threads, timing off | 53.7 | 89% | - | 1150 MHz | 15.3 ms | 3.7 ms |
| three host threads at 30, timing on | 28.0 | 93% | 21.7 | 859 MHz | 18.0 ms | 1.45 ms |

**The ~18.7 ms is measured now, not inferred:** the GPU takes 18.7 ms a frame, and with three host threads 53.1 fps
is 18.8 ms a frame: the GPU is busy all the time, the game thread waits on it (the fence wait 4.1 ms). The timing
itself costs ~1% (53.7 fps without). **The power cap doesn't slow the GPU**: its clock holds 1150 MHz, its top,
with one host thread or three (one busier CPU core more changes nothing measurable). At 30 it's idle part of each
frame and clocks down (859 MHz), so each frame's work takes longer (21.7 ms) at a lower clock: 30's numbers are no
guide to 60's.

**By kind, ms a frame (gameplay, three rounds' lines), against the desktop worker's (b2b9f24, 3800 frames):**

| kind | worker iGPU | desktop GPU | ratio |
|---|---|---|---|
| passes | 11.4 | 0.84 | 14x |
| copies (CopyOf) | 4.5 | 0.20 | 22x |
| clears | 0.98 | 0.07 | 14x |
| scan copy | 0.63 | 0.02 | 30x |
| mips (ChainOf) | 0.58 | 0.10 | 6x |
| resets | 0.53 | 0.04 | 13x |
| other (barriers, gaps) | 0.05 | 0.11 | |
| **total** | **18.7** | **1.38** | **13.5x** |

**Reconciled:** the desktop worker's discrete GPU is simply ~13.5x the worker's iGPU on this frame, and the gap
is widest on the work that only moves memory (copies 22x, the scan copy 30x): the iGPU shares the CPU's system
memory. The 1920x1080 copies run at ~28 GB/s (8.3 MB read and 8.3 MB written a copy, 0.59 ms each), about what
that memory gives. So on the iGPU a byte not moved is worth more than a shader instruction saved: the copies, the
clears and the loads of targets (item 2) come before the shaders (item 5).

**The passes and copies that cost most (ms a frame, times a frame), the run on one round:**

| work | ms | per frame |
|---|---|---|
| pass 1920x1088, one colour target f64 (A2B10G10R10) | 2.52 | 4.2x |
| pass 960x544, one colour target f37 (RGBA8): half-size effect buffers (ambient occlusion's size) | 2.06 | 3.7x |
| **copy 1920x1080 <- 1920x1088 f64 (CopyOf: the height padded for tiling)** | **2.02** | 3.4x |
| pass 1920x1088 f64 with depth (the scene) | 1.66 | 5.0x |
| pass 1920x1088, two colour targets f64 with depth | 1.40 | 2.2x |
| **copy 1920x1080 <- 1920x1088 f126 (D32, the depth sampled)** | **0.77** | 0.9x |
| **copy 1920x1080 <- 1920x1088 f9 (R8)** | **0.68** | 1.9x |
| pass 960x544 f64 | 0.66 | 1.9x |
| pass 1920x1088 f9 | 0.57 | 1.0x |
| pass 480x272 f122 (B10G11R11) | 0.56 | 1.0x |
| copy feedback 1920x1080 <- 1920x1088 f126 (a draw samples its own depth) | 0.54 | 0.7x |
| pass 960x544 two colour targets f64 with depth | 0.49 | 0.9x |
| pass 1024x1024 depth, 3 layers (the shadow maps) | 0.41 | 2.2x |

**What it says for 60 here** (16.7 ms, 2 ms under today's 18.7): the size copies of 1920x1088 surfaces sampled as
1920x1080 are 3.5 ms a frame by themselves; `WWHD_SURFACE_FIT` removes them (item 2: b-barriers' A/B), the
feedback copy stays (it's a real read of the target being drawn). The half-size 960x544 RGBA8 passes (2.06 ms) are
the ambient occlusion candidate (b-gfxopts). Clears and resets (1.5 ms) are b-clears'.

## Item 2: narrow barriers and surface fit, on by default (session bottom, 2026-10-09)

`WWHD_BARRIERS=narrow` (session cloud2, 243d4ae) and `WWHD_SURFACE_FIT=1` (1b97d37, 32c3631) are the defaults now;
`WWHD_BARRIERS=all` and `WWHD_SURFACE_FIT=0` give the old behaviour. The worker's A/B (`worker-ab.sh`, one binary,
4 rounds after a warm-up, the lazy DrawDone and three host threads in every variant, `WWHD_GPU_TIMING=1` in all):

| route | variant | fps | game speed | GPU ms a frame |
|---|---|---|---|---|
| continue at 60 | old (all barriers, no fit) | 52.4 | 87% | 18.89 |
| | narrow barriers | 52.8 (+0.6%, noise) | 88% | 18.76 |
| | surface fit | **60.0 (+14.6%, every round)** | **100%** | 16.06 |
| | both (the default) | **59.9 (+14.5%)** | **100%** | 16.09 |
| en-tn (the Darknut fight) at 60 | old | 44.1 | 74% | 21.76 |
| | narrow barriers | 44.2 (+0.7%, noise) | 74% | 21.77 |
| | surface fit | 50.2 (+12.0%) | 84% | 18.38 |
| | both (the default) | 49.1 (+11.5%) | 82% | 18.77 |

**Surface fit makes the worker hold 60 at full speed on the continue route**: the CopyOf size copies (1920x1088
surfaces sampled as 1920x1080, ~3 ms a frame on the iGPU) are gone. The Darknut fight still needs ~3 ms more
(item 3's dynamic resolution, b-clears). **Narrow barriers buy no measurable speed** on this GPU (it runs one queue's
work largely in order anyway), but they stay on: they are what the spec asks (the default's write-after-write orders
by chance, 71 synchronization-validation hazards on the continue route; narrow, with surface fit: 0).

Correctness: `tools/sixty/tests/fit_proof.sh` (`WWHD_SURFACE_FIT=proof` over predeploy's 50 routes and gohmatail,
gohmarock, en-tn, route, tour: 0 reads past the fitted rows on all 55); the checks with both on: all MATCH, the
captures byte-identical (the surfaces are fitted mid-run, before the later captures); regress identical; predeploy
0 FAIL.

## Item 2's rest: clears as load ops, on by default (session bottom, 2026-10-09)

A clear of a whole single-layer image (the game's colour and depth clears, the depth clear's colour aliases, the
overwritten surfaces' resets at a swap) is kept on the image (`DeferClear`) and done by the load op of the next pass
into it when that pass covers the whole image (`VK_ATTACHMENT_LOAD_OP_CLEAR`); any other use records it first
(`FlushClear`, from `Transition`, a sample of a surface already in its layout, a rescale). `WWHD_CLEAR_LOADOP=0`: as
before. The worker (4 rounds): continue at 60 GPU 15.79 -> 15.49 ms a frame (-1.9%, every round; fps at 60 either
way), en-tn +0.4% (noise). On the desktop GPU clears went 0.07 -> 0.02 ms and resets 0.05 -> 0, the passes taking
most of it over. Checks with it: all MATCH, captures byte-identical; synchronization validation 0 hazards. The
timing names the clear and reset spans now: the costly ones are one colour+depth clear of the 1920x1080 targets a
frame, the 864x480 GamePad screen's clear, and a reset of a 1920x1088 target. Redundant transitions, the plan's
other half: barriers and gaps are 0.05 ms of the worker's 18.7, nothing to win.

## Item 3, render scale: built (session cloud3), opt-in

**`WWHD_RENDER_SCALE=0.5..2`** (renderer.cpp `RenderScale`; 1, the default and every check's, changes nothing:
every image is the guest's size, the code below multiplies by 1). The screen-sized render targets (guest size at
least 1280x720: the scene's 1920x1088 targets and their full-screen passes, nearly all the pixels drawn) are made
at the scale; everything the guest sees stays in its sizes:
- `Image::gw/gh` (renderer_internal.h): the guest's size an image stands for, `width/height` its own; surface
  lookups and growth, aliasing (the depth clear's colour textures), WWHD_SURFACE_FIT's rows and the texture-against-
  surface size checks use gw/gh, and the Vulkan copies the images' own sizes;
- viewports and scissors (draw.cpp) are scaled by the target's width/gw (the scissor outward to whole pixels), the
  render area is the attachments' own;
- copies of a surface (CopyOf), mip chains taken from surfaces (ChainOf) and the scan images are made at their
  surface's scale (a chain level at another scale is blitted); the window's present scales the scan image as before;
- the shaders' `uf_fragCoordScale` (guest over own size) and `uf_texNScale` (own over guest, per texture unit) are
  filled as Cemu's resolution packs fill them. None of the 632 shaders the Outset captures meet reads either, nor
  `textureSize`: they sample with coordinates from their vertices, so the scale is invisible to them.

**Why only the screen-sized targets.** Scaling every surface (the first try) weakened ambient occlusion at 0.5:
its half-size buffers (960x544) are sampled with offsets sized in their own texels, half a texel apart once
halved, and the objects got a light halo (the TV image 1.9 brighter on average; the dumped AO buffer at PSNR 22
against 1x's). The half-size effect buffers and the shadow maps (1024x1024, three slices) now keep the guest's
sizes; they cost little next to the full-size passes, and a pass mixing scaled and unscaled targets is logged
("render scale: a pass into ...", never seen on the routes tried).

**Results (Outset, the continue route's dock):** the TV image upscaled back to 1080p against 1x is as close as
1x resampled down and up again: 0.75 PSNR 36.8 / 34.5 (resampling alone 36.9 / 35.5), 0.5 33.1 / 31.3 (33.4 /
32.0), the mean brightness within 0.2; by eye only softer. GPU time a frame on the desktop worker's GPU
(WWHD_GPU_TIMING, the same 2400 frames): 1.44 ms at 1, 1.29 at 0.75 (-10%), 1.18 at 0.5 (-18%); that GPU isn't
fill-bound here (the 1024x1024 shadow pass, 0.15 ms, and the fixed costs stay). The settings page's "Render scale"
(100/75/50%, Auto: dynamic resolution, at the next start); the Performance preset sets Auto (it was 75% until
2026-10-09: Auto keeps 100% where the GPU keeps up and holds 60 where it doesn't).

**Dynamic resolution (session bottom, 2026-10-09), opt-in: `WWHD_RENDER_SCALE=auto`** (the settings page's Render
scale "Auto"). The scale moves between `WWHD_RENDER_SCALE_MIN` (0.5) and `_MAX` (1) in steps of 1/8 from the GPU's
time a frame against a budget (`WWHD_RENDER_SCALE_BUDGET` ms, default 92% of a frame at the frame rate): down at once
when a half-second window runs over it (the step from the estimate that all the time scales with the area), up one
step when that estimate at the next step stays under 85% of the budget for two seconds. The GPU's time comes from
gpu_timing's light mode (a timestamp at each command buffer's start and end; no lines unless `WWHD_GPU_TIMING=1`).
A change rescales every scaled surface at the swap (`RescaleSurfaces`: blitted, linear; depth and formats that can't
be blitted start from zero, as a grown surface does); mip chains are made again at their surface's scale, the scan
image follows its surface's. Every image keeps the scale it was made at (`Image::scale`), and copies, chains,
blits, viewports and scissors use the image's own, so surfaces at two scales can meet for a frame. Why not render
into part of a full-size target instead (no reallocation): the game's shaders sample with normalised coordinates
and read neither `uf_texNScale` nor `textureSize`, so a part-filled target would be sampled whole.
Tested: a virtual-clock run stepping 1 -> 0.75 at frame 61 gives captures byte-identical to a fixed 0.75 run at
frames 1000, 1300, 1600 (tour3); real time under synchronization validation, four steps 1 -> 0.5: 0 hazards.

**The worker's A/B (session bottom, 2026-10-09; lazy DrawDone, three host threads, 3-4 rounds, every run of a
variant above every run of 100%'s):**

| en-tn (the Darknut fight) at 60 | fps | game speed | GPU clock |
|---|---|---|---|
| 100% | 49.9-50.0 | 83% | 1147 MHz |
| 75% | 56.2-56.6 (+12%) | 94% | 1064-1077 MHz |
| 50% | 59.7 (+19%) | 100% | 804 MHz |
| Auto | 59.2 (+18%) | 99% | 816 MHz |

Continue at 60 already holds 60 at 100% (60.1 fps); Auto there 59.9, stepping between 100% and 87.5% where a window
runs slow. Two things the runs showed: **at 50% the iGPU has time to spare and clocks itself down** (804 MHz), so its
time a frame rises again; the first controller, deciding from the GPU's time alone, kept stepping down. It now
decides from missed frames (above). And **a pass mixes a 960x544 target with a scaled 1920x1088 one** once on en-tn
(frame 2050, at any scale below 1, cloud3's log line): the captures around it at 75% against 100% are as close as
before it (PSNR 37.7-37.9 upscaled, the same picture by eye), so it's left as a logged case.

## Item 4, the settings menu: design (session cloud3; the first round landed as designed: handoff.md, "Session cloud3")

**What exists.** Every option on the list is a startup switch today, read once from the environment (`static const`
getenvs): `WWHD_60FPS` (dispatch.cpp `SixtyFps`), `WWHD_LAZY_DRAWDONE` (renderer.cpp), `WWHD_VSYNC` (present.cpp's
present mode), `WWHD_CORES` (coreinit_Thread.cpp, session cloud's branch), fullscreen (window_system.cpp, F11 live).
Not there yet: render scale (item 3), ambient occlusion off (needs the AO pass found: item 1's timing names the
passes), the shader set (item 5). The debug menu (debug_menu.cpp: pages, cursor, pad/keyboard/mouse; the overlay
draws any `View`) is the machinery.

**The settings file.** `portable/wwhd.ini`, `KEY=VALUE` lines, the keys being the switches' own names
(`WWHD_60FPS=1`, `WWHD_LAZY_DRAWDONE=1`, `WWHD_VSYNC=0`, `WWHD_CORES=3`, later `WWHD_RENDER_SCALE=0.75`...). Read at
the very start (cemu_boot.cpp `SetupPaths`, before anything reads a switch) and applied with `setenv(key, value,
0)`: **a switch set in the environment wins**, so tests, `play.sh`'s own and the owner's command lines are unchanged.
Loaded only in real time with a window (`WWHD_WINDOW` not 0, no `CEMU_VIRTUAL_CLOCK`/`REF_VIRTUAL_CLOCK`): the
checks, regress, predeploy and the A/B scripts never see it (their portable dirs are fresh anyway). So every option
keeps one code path, its switch, and the menu only writes the file. `WWHD_SETTINGS=0` ignores the file.

**The menu.** F2 (and an item on the debug menu's top page) opens a "Settings" page in the same panel: one line per
option, `Frame rate: 60`, `Vsync: off`, ...; A/Enter/click cycles the value (left/right too), the file is written at
once. Each option says when it applies: **live** (fullscreen and window size: SDL; vsync: a swapchain rebuild with
the other present mode, present.cpp already rebuilds on resize) or **at the next start** (frame rate, host threads,
lazy DrawDone, render scale, shader set: read once at start; the line shows "(restart)" while the file differs from
what runs). No live 30/60 switch at first: SixtyFrom is fixed per run and the half-tick machinery assumes it; a live
switch is a later item if the owner wants it.

**Presets (landed, the owner's ask):** Performance turns on every speed option (CPU threads 3, lazy DrawDone,
keep speed when frames dip, render scale Auto (dynamic resolution); later AO off), Quality keeps the defaults, Auto
picks Performance on a low-powered device (handoff.md, "Session cloud3"). Each new speed option gets its Performance
value in settings.cpp's table.

**The options, first round:** frame rate 30/60; vsync on/off; display: windowed/fullscreen and a window size
(1280x720, 1920x1080, 2560x1440, the desktop's); host threads 1/3 (once `WWHD_CORES` lands); lazy DrawDone on/off.
Added as their items land: render scale (0.5-1x, item 3), ambient occlusion on/off (after item 1 finds its pass),
shader set (item 5). Defaults are today's (no file: nothing changes).

**Checks.** The file is never read with the virtual clock, so checks/regress/predeploy are unchanged by construction;
a unit-ish test: a headless real-time run with a file setting `WWHD_60FPS=1` presents at 60 (its frame log), and the
same with `WWHD_60FPS=0` in the environment presents at 30 (the environment wins).

Who (2026-10-08): session cloud finishes the three host threads (soak, gates, `WWHD_CORES=3` landed opt-in) and the
fight scene's numbers; session cloud2 takes items 1 and 2 (the renderer) once its write-watch branch lands; session
cloud3 continues the deck-plan's CPU items (5: the journal, 7: Link's half step), then the settings menu (4). Timing
runs on the worker go through session cloud.

## Frame dips without slowdown (the owner, 2026-10-08)

Today a pair of frames (whole tick, half tick) that overruns its two vsyncs starts the next pair late, so a dip slows
the game (pacing.cpp). The owner wants dips to look like dips (fewer frames), not slow motion: 45 fps should play at
full speed. Design: in real time, when a frame is behind its schedule, the next frame is a whole tick (its half tick
skipped) until caught up, so the game keeps 30 ticks a second and only the in-between frames go. The first step of
the uncapped phase (D21 step 4); 30 and the checks (virtual clock, never behind) unchanged. Session cloud, after
`WWHD_CORES=3` lands.

**Built (session cloud), opt-in: `WWHD_60FPS_KEEPSPEED=1`** (src/overrides/pacing.cpp). The ticks keep a schedule
(33.3 ms each, resynced after a hitch of over 250 ms); a half tick's frame that would end after the next tick is due
(the half frames' average work from now) is dropped: its logic and actor draws run, the game's render jobs
(RenderDisplay draw and calcGPU) and its present don't; its swap is counted, so the frame numbers and the whole/half
rhythm stay. The actor draws stay because half-tick draws take numbers from the game's random stream
(`WWHD_60FPS_DROPDRAWS=1` drops them too: on the virtual clock the seagull's flight then parts at once).
`WWHD_60FPS_DROPTEST=k` forces every k-th half frame dropped (a test); `tools/sixty/tests/droptest.sh` compares
routes on the virtual clock with and without (Link 0.0 apart on tour3 and en-bo, 16 units on sail; the seagull parts:
timing moves the shared random stream). Real time on the worker:
- the Darknut fight with three host threads and the lazy DrawDone: 44.9 fps at 75% of the game's speed -> **39.9 fps
  shown at 100%**; without the lazy DrawDone the worker's GPU is too slow even for whole ticks (58%);
- routes at 60 in real time with half of all half frames dropped (DROPTEST=2, three host threads): Link's path
  identical to plain 60 on 7 of 8 (sail: real time's own noise either way);
- gates: checks all MATCH (PSNR inf), regress identical, predeploy 51 ok (gohmatail WARN 54.0 as on main).
