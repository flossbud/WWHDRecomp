# B76: the 60 fps crash in the Darknut fight (fixed: a test aid's bug, not 60 fps)

Session cloud found and probed it (2026-10-09); session cloud5 found the cause and fixed it the same day. The probe
`WWHD_J3D_GUARD` stays in `src/overrides/sixty.cpp`, off by default (below). Session cloud's display-list probe
(`WWHD_GX2_GUARD`, src/os/gx2/core/GX2_Command.cpp) stays on the worker remote's branch `ww-4-cloud-gx2guard`, not on main.

## The symptom

- Route `en-tn` (a Darknut spawned by the route's `WWHD_DEBUG_SPAWN` at game frame 950, Link slashing it) at 60 fps
  in real time on **the worker** (the Deck stand-in: 45 W CPU, Intel UHD 630): 25-40% of runs died with SIGSEGV at
  `f_025C91FC+0x3a31` (a guest load through a register restored from an overwritten stack frame).
- With and without the lazy GX2DrawDone, with one host thread and with three. Not seen at 30 (4 runs: too few), nor
  on the desktop worker (24 runs).
- Always 1.1-1.2 s after the spawn (swap 1950, game frame 975).

## The cause

1. **The spawn's layer.** `WWHD_DEBUG_SPAWN` (`SpawnNow`, sixty.cpp) creates the actor as fopAcM_create does
   (f_025D5834): the creation record (f_025D5678), then fpcM_Create (f_025E14A8) into the current layer
   (fpcLy_CurrentLayer, f_025DED64: `*0x101F3AE8`). The process manager sets the current layer to each process's
   own layer around its execute and draw (fpcLy_SetCurrentLayer, f_025DEAB4). Inside an actor's execute, where the
   game calls fopAcM_create, it is therefore that actor's scene layer. The spawn runs at the start of the frame
   body (f_025F172C), outside any process, where the current layer is whatever was set last.
   - On Outset the layers are: the root (`*0x101F3B18`, id 0); the play scene's (id 6, Link's: process+0x2C, his
     layer tag's); and two room scenes' (ids 7 and 8, process 18).
   - In clean runs the spawn got a room's layer (id 7): 4 of 4 logged before the fix. The failing run logged got
     **the root layer**. With the fix, 7 of 24 runs found the root layer at the spawn (29%, the old crash rate).
   - Which one is left over depends on real-time timing. It wasn't traced further.
2. **Drawn twice.** A Darknut created in the root layer sits on the root layer's process lists (its layer tag's node
   at process+0x18, on list 7) and is also drawn by the play scene's walk of its own layer. The draw pass drew it
   twice a frame: once from the root layer's walk (f_025DF908 -> f_0201ABF0 -> f_02019D50, list next at +8), once
   under the scene node's draw (current layer = the play scene's).
   - So did the sword it creates (BOKO 463, from its own execute, so in the root layer too).
   - Only the failing runs drew a process twice (3 of 3 checked): from swap 1906, the Darknut's first draws; none of
     the 8 clean ones did.
3. **A J3DDrawBuffer list loops.** Each draw enters the actor's model packets (vtable 1016e324: J3DModel's entry
   f_027F4F1C, through the buffer's sort table at 0x101F9898) into the opaque buffer. Five of its six sort types are
   entryNonSort (f_027F0DA8), which like entryZSort (f_027F0C14) links a packet without checking that it is on a
   list already. Only entryImm (f_027F0E04) asserts (J3DDrawBuffer.cpp 243). The second entry in a frame sets the
   packet's next to the list's head: itself, or a packet that leads back to it. At swap 1950 the sword's packet went
   on again after the play scene's 4 packets (process 7, the draw manager's among them), so those 5 made the loop.
   - The Darknut's draw holds back its model until about swap 1950. The sword's model came first (swap 1950: entered
     once early in the pass, again at its second draw), then the Darknut's 8 packets (1951 on).
4. **The display list overruns.** J3DDrawBuffer::drawHead (f_027F10CC: each list's packets, vtable +0x2C, next at
   +0x10) never ends. It runs on the main thread (f_0273172C) and on the render task (f_02737D54), the same lists,
   so both looped. The loop includes the big draw-manager packet (f_025C91FC's draw, made by f_025C7A1C), which
   applies render-state blocks (f_02750370): the words session cloud found on the stack. Both threads' display lists (4 MB regions) filled within 60 ms and
   went on past their end: GX2's display-list flush is a no-op and Cemu's write-gather writes on. The words landed
   on a thread's stack: the SIGSEGV.

Nothing in this is 60 fps's: the same spawn at 30 would do the same if it found the root layer current (not seen in
session cloud's 4 runs at 30; which layer is left over depends on the run's timing, and the worker's made the root
common at 60). Nor the half tick's rollback or hide: they are untouched by the fix, and the runs pass.

## The fix

`SpawnNow` keeps the current layer as it was (a room scene's in every clean run and in the gates' runs under the
virtual clock, which would hang otherwise: their results stay identical). When that is the root layer, it makes
Link's layer the current one for the creation and puts it back after, as in his execute, and the spawn line says
so ("in Link's layer"). The debug menu's spawn (`RequestSpawn`) shares it.

Gates (on main 32c3631): checks all MATCH (traces, streams, sound), diff 0 mismatches, captures PSNR inf; regress
identical to a main build's; predeploy 50 ok, 0 FAIL, gohmatail WARN 54.0 (as on main), gohmarock ok.

## The probe (left in, off): `WWHD_J3D_GUARD`

- `=1` logs ("j3d guard:" lines, up to 60):
  - a packet entered while its slot (+0x94) says it's on a list already (the three entries are overridden), with
    the guest call chain and the process being drawn;
  - a loop on any list before it is drawn (drawHead, drawTail f_027F1174: Brent's);
  - a process drawn twice in one draw pass (fpcM_Draw), with the current layer and the chain of both draws.
- `=2` also leaves the second entry out and cuts a loop. That made the failing runs finish, which is how the
  cause was found.
- Off: one predictable branch per entry and draw.

## Evidence (all en-tn at 60 on the worker, one host thread, lazy DrawDone, game data kept on the worker)

| round | runs | what | result |
|---|---|---|---|
| j3d1 | 10 | `WWHD_J3D_GUARD=2` (entries) | 2 runs with double entries, both from swap 1950, the same 9 packets; no crash |
| j3d2 | 11 | + each buffer's last frameInit and draw | 2 failing: buffer 47d49b08 reset by the main thread at 1950 (dDlst reset f_0252F264, from fpcDw_Handler's before-draw) |
| j3d3 | 8 | + the buffer's event trace | 3 failing: the sword's packet entered at 1950 after the reset, then again later in the same pass |
| j3d4 | 4 | + the process lists checked at frame start, draw start, frame end | 1 failing, the lists consistent at all three |
| j3d5-6 | 5 | + a process drawn twice, both chains | failing runs only: the Darknut drawn twice from swap 1906, once under the play scene's draw, once from the root layer's walk |
| j3d7 | 5 | + the spawn's layer | the failing run spawned into the root layer, the 4 clean ones into a room's |
| fix1 | 24 | the fix, probe off | **24 clean**; 7 of them spawned at a root current layer (moved to Link's) |
| fix2 | 24 | the fix rebased on main (32c3631), probe off | **24 clean**; 14 of them at a root current layer |

Before the fix the rounds of session cloud and cloud5 crashed or showed the loop in 25-40% of runs. With it, 48 runs
in a row were clean, 21 of them in the case that crashed every time before (24 clean runs at a 30% rate: p = 0.0002).

## A placed Darknut

Route `gantn` (tools/reference/routes/gantn-100.txt): Ganon's Tower's Darknuts (GanonL room 0, ACTR: placed by the
game, no spawn), warped to with the 100% save at spawn point 1, which faces two of them 200 units off. Their switch
26 is forced on: without it they stand still on this save (walked 0 units). With it both walk up and fight.
- 12 runs at 60 on the worker (`WWHD_J3D_GUARD=1`, the flight recorder on the Darknuts and Link): **12 clean**.
  Each run had two Darknuts walk 176-381 units up to Link, closest 135-245 units, within 250 units for up to 185
  frames. The probe logged nothing: no double entry, no list loop, no process drawn twice.
- (The Tower of the Gods' placed Darknut, Siren room 23, isn't there on the 100% save.)

## Reproduce (before the fix)

```
git checkout <a commit before the fix>; tools/worker/sync.sh; tools/worker/job start b76 bash -c 'src/build.sh && \
    PERF_TAG=b76 PERF_KEEPLOG=1 PERF_WARMUP=0 PERF_NOWAIT=1 tools/sixty/perf/worker-ab.sh en-tn:1100 12 \
    g:WWHD_LAZY_DRAWDONE=1,WWHD_J3D_GUARD=2'
# each run's log: $OUT/perf/b76/ab-g-en-tn-N.log; grep "j3d guard" (worker-ab.sh's PERF_KEEPLOG=1)
```

`worker-ab.sh` sets `ulimit -c 0` (core dumps hold game memory; `PERF_CORE=1` allows one: read it in place on the
worker, then delete it). Nothing from the logs beyond small numbers and addresses leaves the worker.

## Session cloud's rounds (before the cause was known)

| round | runs | crashed | notes |
|---|---|---|---|
| 1 | 24 (half ROLLBACK=1) | 4 | `WWHD_GX2_GUARD` covered only the U32 writers: silent |
| 2 | 16 (half ROLLBACK=0) | 3 | every crash overflowed a display list, no clean run did |
| 3 | 12 (60, ROLLBACK=0, 30) | 2 | both with ROLLBACK=0; 30: 0 of 4; display lists normally 0-36K words |
| 4 | 8 | 1 | spill on: both threads overran in the same swap; died anyway |
| 5 | 3 (stopped) | 1 | the render task's call chain at the overrun |
| 6 | 2 (stopped) | 1 | the main thread's chain: both threads in f_025C91FC under drawHead (027f112c) |
