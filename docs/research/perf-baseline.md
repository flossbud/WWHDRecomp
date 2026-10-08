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

## Changes measured against this baseline

- **ProgramHash's copy check** (src/gpu/vk/draw.cpp, session qa): a program's first use in a frame compares it with
  a copy of its bytes (memcmp) and hashes it again only when it changed, and the per-draw table has 4,096 slots, not
  1,024. Exact (the hash is the content's, as before): checks' captures byte-identical (PSNR inf), predeploy all ok.
  ProgramHash 9.1% -> 3.1% of the render thread's samples (continue at 60). `perf-ab.sh` at 60, three alternating
  rounds each (render thread per frame, gameplay): continue 3.20 -> 2.98 ms (-7%), house 3.60 -> 3.28 ms (-9%);
  every run with it below every run without.
- **Index decoding's cache** (src/gpu/vk/draw.cpp DecodeIndices, session bottom): a draw's decoded indices are kept
  per (source address, count, index type, quads or quad strip, output width, restart index) with a copy of the
  source's bytes; a draw whose source still holds them (memcmp) copies the entry's decoded indices into the ring
  instead of decoding, so the result is the decode's exactly; auto-generated quads (no source) by the key alone;
  cleared past 64 MB; `WWHD_INDEXCACHE=0` off, `WWHD_RENDER_STATS=N` counts reuses. On house 3,058,042 of 3,058,580
  index decodes reused by frame 2,400 (538 decoded). Profile, continue at 60: `Decode<uint>` + `Decode<ushort>` 5.9%
  self -> gone; `DecodeIndices` 1.0% self, 2.9% inclusive (its memcmp and copy). `perf-ab.sh` at 60, three
  alternating rounds, the same binary with `WWHD_INDEXCACHE=0` against on (render thread per frame, median of each
  run): continue 3.40 / 3.18 / 3.95 -> 2.90 / 3.33 / 2.91 ms, house 3.22 / 3.25 / 3.30 -> 3.30 / 3.12 / 3.22 ms:
  within the runs' noise on house, about -0.3 ms on continue. Exact: checks' captures byte-identical, predeploy ok.
- **The sampler cache's key word by word** (src/gpu/vk/texture.cpp `Sampler`, session qa): SampleTexture's own
  hottest loop was the sampler key, a byte-at-a-time FNV over ~56 bytes of the create info for every texture of every
  draw (about a third of its self time; found from the profile's hot addresses and the disassembly, the build has no
  line info). Now eight bytes at a time: the same fields, the same samplers. SampleTexture 7.2% -> 5.6% of the render
  thread's samples; the A/B (continue 3.02 ms both, house 3.24 -> 3.28) doesn't resolve a change this size (its runs
  spread ~0.2 ms). Captures byte-identical, predeploy all ok. Its other hot loop (~19% of its self time) is the
  texture memory check (SampleMemory/HashMemory), already sampled in real time.
## The half frames' cost on the game thread, split (session bottom)

`WWHD_PROFILE` on the desktop (13700K, real time, headless, the build after the index cache), continue (to f1800)
and en-tn (the Darknut fight, to f1240) at 30 and at 60: the same game ticks over the same real time, so the game
thread's (`OSSched`) samples at 60 less those at 30 are what the half frames add. Each sample is put under the
actor execute it is in (fopAc_Execute f_025D475C's callee, mapped to its process type through the profile tables),
the actor draw (fopAc_Draw f_025D4654's callee), another process's execute or draw (the scenes, the environment:
fpcM_Execute/fpcM_Draw without an actor), or the rest. Samples at 30 -> 60 (+ added):

| part | continue | en-tn |
|---|---|---|
| the game thread | 12,738 -> 21,980 (+9,242, +73%) | 8,125 -> 14,832 (+6,707, +83%) |
| actor executes and other processes' executes | 1,593 -> 2,835 (+1,242) | 951 -> 1,892 (+941) |
| actor draws and the scene's draws | 1,676 -> 3,060 (+1,384) | 998 -> 2,176 (+1,178) |
| the rest (the frame body, the journal, sound, the fibers' runtime) | 9,469 -> 16,085 (+6,616) | 6,176 -> 10,764 (+4,588) |

So the converted processes' half-step executes are only about 13-14% of what the half frames add, their draws
about as much again (every actor draws on every frame), and most of it is outside both. In the rest, by what 60
adds (inclusive, continue): the sound's AX processing on the game thread (`snd_core::AXIst_ThreadEntry` 413 ->
2,191, `PPCCore_executeCallbackInternal` 328 -> 1,770, `AXMix_process` 139 -> 744: about five times its 30 cost,
the largest single item; found and fixed below), the half tick's
frame path (`f_0274C264` 963 -> 1,830; on en-tn 637 -> 1,456) and the journal (`rt_journal_store`, en-tn 5 ->
387).

The top 10 actor executes by what 60 adds (samples 30 -> 60):

| continue | | en-tn | |
|---|---|---|---|
| 168 PLAYER (Link) | 299 -> 538 | 168 PLAYER | 169 -> 366 |
| 200 | 45 -> 134 | 38 | 25 -> 90 |
| 194 (the seagull) | 85 -> 171 | 194 (the seagull) | 56 -> 119 |
| 165 SHIP | 58 -> 137 | 89 BRIDGE | 26 -> 80 |
| 38 | 57 -> 130 | 200 | 30 -> 77 |
| 220 KB (the pigs) | 54 -> 120 | 220 KB | 39 -> 84 |
| 453 TSUBO (pots) | 36 -> 99 | 165 SHIP | 24 -> 57 |
| 118 NPC_SO (the fishman) | 31 -> 88 | 191 TN (the Darknut) | 26 -> 59 |
| 182 KANBAN (signs) | 39 -> 89 | 68 OBJ_IKADA | 37 -> 70 |
| 89 BRIDGE | 31 -> 74 | 453 TSUBO | 43 -> 76 |

The top actor draws (every actor's draw runs on every frame; the half frame's count is the whole's): continue
PLAYER 125 -> 237, BRIDGE 86 -> 190, 38 69 -> 141, 445 39 -> 104, 439 76 -> 138, KNOB00 69 -> 125, VRBOX2 93 ->
137, SHIP 37 -> 62; en-tn BRIDGE 80 -> 200, PLAYER 69 -> 139, 38 32 -> 87, 439 36 -> 89, VRBOX2 44 -> 91,
Obj_Lpalm 23 -> 60, KNOB00 31 -> 65. The scene's own draw (f_025DFFB0 under fpcM_Draw: the play scene's draw, its
collision and packets) 674 -> 1,165 on continue, 431 -> 868 on en-tn.

What it suggests: Link is the one actor worth making cheaper in his half step (his execute about doubles; most
types are a few tens of samples each); static or far actors (signs, bridges, palms, door knobs, the sky box) still
draw on half frames and could reuse their whole frame's matrices; and the sound's cost at 60 is the first thing to
look at, before any game code. (The profiles and the scratch scripts that split them, `halfsplit.py`,
`callees.py`, `mapcallees.py`: the desktop worker's data/m6/WWHDRecomp-bottom/prof and bin.)

### The sound's cost at 60: the half tick's journal on the AX thread (fixed)

The AX frames are the same at both rates (one each 3 ms of host time: ~17,000 in 49 s at 30 and at 60), with the
same voices (~2 a frame) and the same work in the game's stream mix (f_0281A540: 96 samples a call, 2 streams, the
same calls) - probes `WWHD_AX_STATS` and a stream-mix override, not kept. But the AX thread is a guest thread on the
same core, and the half tick's store journal (`g_rtJournalOn`, global) was on whenever an AX frame ran during a half
tick's frame: every store of the game's audio code went through `RT_STORE`'s page check and `rt_journal_store` into
`HalfTickStore`, whose rollback drops other threads' stores anyway. With the journal off (WWHD_60FPS_ROLLBACK=0, a
probe) the AX thread fell from 2,191 to 333 samples. Fix (src/runtime/diff.cpp, sixty.cpp): the half tick's journal
names its thread (`g_rtJournalThread`, the frame thread) and `rt_journal_store` passes other threads' stores over at
once, unless a fast path's quiet watch needs them. AX on continue at 60: 2,191 -> 287 samples (30: 413). Exact: the
stores passed over were dropped before; checks, regress, predeploy and the save route's sound hash at 60 unchanged.
