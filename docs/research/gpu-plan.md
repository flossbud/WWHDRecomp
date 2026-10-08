# The GPU side: 60 on weak GPUs, and a settings menu (the owner's plan, 2026-10-08)

Why: on the worker (the Deck stand-in) the CPU side of 60 now fits (three host threads and the lazy DrawDone: the game
thread 15.3 ms a frame on continue; docs/research/threads.md), and the frame rate stops at ~54 fps because the Intel iGPU
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

Who (2026-10-08): session cloud finishes the three host threads (soak, gates, `WWHD_CORES=3` landed opt-in) and the
fight scene's numbers; session cloud2 takes items 1 and 2 (the renderer) once its write-watch branch lands; session
cloud3 continues the deck-plan's CPU items (5: the journal, 7: Link's half step), then the settings menu (4). Timing
runs on the worker go through session cloud.
