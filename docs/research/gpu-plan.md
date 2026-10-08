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

## Item 4, the settings menu: design (session cloud3)

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
