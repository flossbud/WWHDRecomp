# Performance baseline before the Steam Deck work (B75)

Session qa, 2026-10-08. A measurement only: nothing in the game path changed. Build 0a10de3 (ww-4), deployed to
the desktop's `~/wwhd-test` and run there in real time, headless (`WWHD_WINDOW=0`), from the 100% save. The desktop
is an desktop CPU (24 threads). It is also the worker, so other sessions' jobs share it: CPU time per frame and per
thread is the measure, not frame times.

Runs (driver: `~/wwhd-test/qa-perf.sh`; outputs in `~/wwhd-test/perf/` and `perf/qa/`, timings only):

- `perf-top.sh` at 30 and then 60: each frame's work, CPU and GPU-thread time (`WWHD_FRAME_LOG`) and each thread's
  CPU over the run's middle half; `perf/perfsum.py` summarises gameplay only (game frame 900 on).
- One profile at 60 on two scenes (`WWHD_PROFILE`, then `tools/profile_report.py` on the worker against the same
  build's unstripped binary, the profile's `exe` line rewritten).
- Draw counts at 30 and 60 (`WWHD_GPU_STATS`, a run of its own: it formats a key per draw, which would cost the
  render thread time in a timed run).

Scenes: `continue` (the save route: Outset from the save, 1800 frames), `house` (Windfall: out of a house's door
and back, the stage warp `sea,9,11`, 1300 frames), `en-tn` (a fight: a Darknut on Outset's grass, 1240 frames).

## Per-thread CPU and frame cost (gameplay only)

Two threads do the work: `OSSched` (the scheduler thread: all guest code, the game) and `NullGPU` (the render
thread: the command processor and the renderer's CPU side; headless it draws offscreen and presents nothing).

| scene | rate | fps | scheduler thread | render thread | whole-tick frame CPU (mean / p90) | half-tick frame CPU (mean / p90) |
|---|---|---|---|---|---|---|
| continue | 30 | 30.1 | 19.3% of a core | 9.4% | 6.27 / 7.37 ms | |
| continue | 60 | 60.1 | 43.4% | 19.0% | 6.46 / 7.65 ms | 7.70 / 9.36 ms |
| house | 30 | 27.7 | 18.0% | 17.7% | 6.28 / 8.82 ms | |
| house | 60 | 59.5 | 35.9% | 21.1% | 6.24 / 8.60 ms | 7.18 / 9.17 ms |
| en-tn | 60 | 59.3 | 57.0% | 36.9% | 8.51 / 10.44 ms | 10.61 / 12.95 ms |

(en-tn at 30 measured 9.3% / 2.6% with a 2.2 ms frame: that run doesn't look like the fight scene and isn't used.
Rerun it before comparing the fight at 30.)

**What 60 fps costs against 30:** the scheduler thread does 2.2x the work (continue 19% -> 43% of a core): two
steps a tick instead of one, and **a half tick's frame costs more than a whole tick's** (continue 7.70 against
6.46 ms, en-tn 10.61 against 8.51), though it runs less of the game: the half step's journal (each store into a
converted process's state is noted, 137k stores a half frame on continue, 178k in the fight) and the rollback after
it. In the profile the journal is ~3% of the scheduler thread (`HalfTickStore` 2.0%, `rt_journal_store` 1.2%), so
most of the half frame is the game's own code run again (the converted processes' half steps, and every draw).
The render thread does twice the work too: every frame is drawn (continue 9.4% -> 19.0%).

## Draws and the render thread's cost per draw

| scene | draws a frame (30 / 60) | pipelines | render thread a frame | per draw |
|---|---|---|---|---|
| continue | 1,126 / 1,133 | 250-256 | 3.10 / 3.15 ms | **2.8 µs** |
| house | 1,228 / 1,219 | 645-648 | 4.37 / 3.41 ms | 3.6 / 2.8 µs |
| en-tn | ~1,700 (both) | 200-328 | 5.28 ms (60) | 3.1 µs |

(Draws a frame: the run's draws over its frames, title screens included. Render thread a frame: the frame log's
`gpu_ms`, gameplay only.)

Against the rival (`docs/research/rival-study.md` §6): their M3 Max does 1.74 µs a draw (5.56 ms for 3,230 draws
on Outset at 60), and a Ryzen 7 7735HS (Zen 3+) about 1.7 µs (11.8 ms for ~6,900). We draw a third as many draws a
frame (1,100-1,700 against 3,200-6,900: our draw count is the game's own, not an emulator's), at ~2.8 µs each on a
13700K, without a GPU wait in the number. So our render thread is about 60% dearer per draw than theirs, and its
total per frame lower because of the draw count.

Where the render thread's time goes (continue at 60, 9,219 samples): the renderer's own code 72%, libc and
unsymbolized libraries 26% (the Vulkan driver: half of all self samples are in code with no symbols), gx2 2%. The
renderer's leaders, self: `ProgramHash` 9.1% (a shader program's content hashed per draw), `SampleTexture` 7.3%,
the PM4 packet parse 4.3%, index decoding 6% (`Decode<uint>`, `Decode<ushort>`), `PipelineLayout` 3.3%,
`Descriptors` 3.0%; inclusive, `RendererDraw` 58%, `Textures` 13%, `Descriptors` 12%. The fight is the same shape
(`ProgramHash` 7.2%, `SampleTexture` 6.4%).

## The scheduler thread's split (continue at 60; the fight is within a point)

| category | share of the scheduler thread |
|---|---|
| guest code (the recompiled game) | 83.8% |
| runtime (the dispatcher, cycle accounting, the context switch) | 4.4% |
| guest code helpers (paired-single loads and stores, `psq_*`) | 3.9% |
| gx2 | 1.7% |
| scheduler (other) | 1.1% |
| HLE dispatch (calls into the OS libraries: `rt_import`'s path) | 1.0% |
| snd_core | 0.9% |
| trace writing (the frame log) | 0.7% |
| OS (other) | 0.7% |
| thread switches | 0.4% |
| message queues | 0.2% |
| mutexes and events | 0.1% |

So the yields, queues and imports are small (thread switches, queues and HLE dispatch together ~1.6%): the game's
own code is the cost. Its top functions (self) are unnamed in `config/US_v0/symbols.csv`: `f_0281B4EC` 5.2%,
`f_0275EBC0` 3.4%, `f_0281B970` 3.4%, `f_02759564` 2.7%, `f_028E8F64` 2.0%, `f_027BF048` 1.4% (the 0281xxxx and
0275xxxx ranges are the engine's, below the actors; naming them is the next step for any guest-side work). Inclusive:
`wwhd::rt::Execute` 22.5% (a fiber's run), the main loop `f_027C25AC` 20.1%, the actor executes (`f_025DE2CC`
14.8%, `f_025DE58C` 13.4%), the play scene's draw (`f_025AF8A0`, collision resolution and more) 13.6%.

## What it suggests for the Deck (estimates, not measurements)

A Steam Deck core (Zen 2, 2.4-3.5 GHz) does roughly half a 13700K P-core's work. On these numbers the scheduler
thread at 60 would need ~85-115% of a Deck core in the fight (57% here) and ~85% on the save route: over budget in
heavy scenes; at 30, ~40%. The render thread at 60 would need ~40-75% of a core before the real GPU wait. That
matches B75's report of the game at 35-55% speed only if something else costs too (the Deck's clocks under the
default power governor: on the worker's `powersave` governor the save route already lost 10% at 30). Measure on the
Deck itself first: the same three scenes, `perf-top.sh`'s per-thread CPU and frame log, a profile, and the CPU
governor and clocks.

Candidates, by what these numbers say:

- the half step's cost (a half frame is ~20% dearer than a whole one): the journal is only ~3%; the rest is re-running
  the converted processes and the draw. A 30 Hz game-logic mode with interpolated presentation would halve the
  scheduler thread at 60 on weak CPUs;
- the render thread per draw: `ProgramHash` per draw (memoise by the program's address and a cheap version, as the
  rival's "shader-state memo"), index decoding (cache decoded index buffers), `SampleTexture` and descriptors
  (skip redundant binds, as their "CPU paths");
- name the top guest functions (`f_0281B4EC`, `f_0275EBC0`, `f_0281B970`, `f_02759564`) before anything guest-side.
