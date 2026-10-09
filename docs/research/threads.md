# Guest threads on more than one host thread (real time only)

Session cloud, 2026-10-08, the owner's choice after the worker baseline (perf-baseline.md, "the worker"). D19 left
"one host thread or three" open; at 60 fps on a weak CPU it is now the largest lever we have measured.

## Why

On the worker (6-core CPU at 45 W, the stand-in for the Steam Deck's CPU), 60 fps with the lazy DrawDone runs at 49 fps and
is limited by the one host thread that runs every guest thread (`OSSched`): 96% busy, 19.5 ms of CPU a frame against
60's 16.7 ms. The Wii U has three cores; the game spreads its threads over them, and we run them all in turn.

**Who uses that host thread** (`WWHD_THREAD_STATS`, a probe in `coreinit_Thread.cpp`: each guest thread's host CPU over
its timeslices; continue at 60, lazy DrawDone, 49 s of CPU):

| guest thread | entry | affinity (cores) | priority | share |
|---|---|---|---|---|
| Default Core 1 (the game's main thread: the frame, actors, draws) | 00e05838 | 1 | 80 | **65.7%** |
| WorkerMgr/Worker2 (two: the job lists, RenderDisplay's among them) | task_ThreadEntry | 2 | 80 | 10.6% |
| JASThread (three: JAudio's sound) | 02810d20 | 0 | 75, 66, 67 | 11.3% |
| an OS thread (entry 00e05890) | | 0 | 14 | 4.1% |
| update_ubo | task_ThreadEntry | 0 or 2 | 82 | 3.1% |
| nw::snd::SoundThread | 028f6af0 | 0 | 68 | 2.0% |
| Prepare Thread | task_ThreadEntry | 1 | 81 | 1.5% |
| ResMgr, Alarm, Olive, Save, IPC, ... | | | | < 1% each |

By affinity: core 1 ~68%, core 0 ~17.5%, core 2 ~11.5%, either of 0 and 2 ~3%. **With a host thread per core the
busiest one would carry ~68% of today's load: ~13 ms a frame on the worker**, under 60's budget, before whatever the
locking costs. The main thread itself (66%) is the floor this design can reach; below it only cheaper game code helps.

## The model: Cemu's three-core mode, made safe for our runtime

Two ways to run guest threads in parallel:
- **The rival's**: every guest thread a host pthread, a per-core ownership lock to keep Cafe OS's priority rules,
  preemption at function entry (rival-study.md §0, §9.3 item 5). A rewrite of the scheduler.
- **Cemu's**: three host threads, one per emulated core, each running its core's guest threads as fibers under the one
  scheduler lock (`OSSchedulerBegin(3)`, `g_isMulticoreMode`). It is still in our fork (`coreinit_Thread.cpp`'s idle
  loop and core choice), the guest's own thread affinities decide the split (the table above), and Cemu ships it as
  its default for this game.

**We take Cemu's.** It keeps the fibers (native frames on guest stacks, D6), the forked OS code (Mutex, Event,
MessageQueue, ThreadQueue, alarms: all under the scheduler lock, written for this mode), and the one-host-thread
deterministic mode untouched: the checks keep `cpuMode = 0` and the virtual clock, so they stay exact by construction.
The three-core mode is real time only, chosen at start (`WWHD_CORES=3`, then the default once it holds).

## What in our code assumes one host thread

From two surveys of the tree (the OS layer and overrides; the runtime and generated code). Cemu's own code is mostly
ready; what isn't is what this project added for real time and for 60 fps.

**Blockers**
1. *The real-time idle sleep* (`coreinit_Thread.cpp`, `__OSIdleWait`, "with one host thread for the three cores") runs
   only when `g_isMulticoreMode` is false: in the three-core mode core 1's idle loop polls without sleeping (cores 0
   and 2 sleep on their run-queue semaphores). `__OSAnyRunnable` checks all three queues and one condition variable is
   woken with `notify_one`. Needed: core 1 sleeps on its own queue, alarms and vsync wake it; the AX cadence, alarms
   and NFC polling stay in core 1's idle loop (correct as is).
2. *The quiet watch and the task loop's sleep* (`dispatch.cpp`: "there is one host thread and at most one watch";
   `g_quiet`, `s_quietTokens`, `QuietStore`; the "left the core" test by `wakeUpCount`). Without it the self-ticking
   task thread (core 1) spins. Needed: the watch per host thread (`thread_local`), ignoring other threads' stores (the
   "seen at most 1 ms later" argument already covers them); "left the core" by the thread's own state.
3. *The `FastPaths()` gate* means "real time" and "one host thread" at once (`override.h`): split it
   (`RealTime()` / `OneHostThread()`), each override on the one it needs (the GamePad view's is thread-safe already).
4. *The 60 fps globals read by every thread*: the half tick's journal (`g_rtJournalOn`, `g_rtStorePages`,
   `g_rtJournalThread`, read in `RT_STORE` on every store; `HalfTickStore` touches `s_page`/`s_savedBits` before it
   checks the thread, racing `JournalPagesClear`), and the step globals set around a converted step (`g_rtStep`,
   `g_rtHold`, `g_rtNote`, `g_rtActor`, ...) that overrides on other threads then act on (`J3DFrameCtrl`'s
   `s_ctrlWhole`, `cM_rnd`'s `s_rndHalf`, the maps in `sixty_step.cpp`). Needed: `thread_local`, set only on the frame
   thread. That gives exactly today's meaning (today a thread that preempts the frame thread mid-step even sees its
   step: a leak this fixes). `g_rtHalfTick` is per frame: `thread_local` too, or read only by the frame thread's code.
5. *The crash at start* (an AX callback the interpreter runs, `PPCInterpreterContainer::executeInstruction`): see
   the runtime survey below.

**Locks, atomics, ordering**
- FSA is served in place on the calling guest thread (`coreinit_FS.cpp`); Cemu's IOSU thread used to serialise its
  client and handle tables: a host mutex.
- The rollback's copies (`HideConvertedGlobals`, `RollbackRestore`) run while other cores may store into the same
  pages: the lost updates are today's (the rollback drops other threads' stores), the copies can now tear. Check that
  the job lists are joined before the frame's end (else render jobs run during the rollback).
- `sync`/`eieio`/`isync` emit nothing: fine on x86 (TSO) but for store-load order; arm64 needs fences.
  lwarx/stwcx. are already a CAS (`emit.py`), so the game's spinlocks hold.
- Counters (`s_nativeEntries`, `s_yields`, `g_rtStoresPassed`, ...): relaxed atomics.
- A fiber can resume on another host thread: `thread_local` addresses cached across a switch (ThinLTO) would be
  wrong (`s_schedulerLockCount`, `t_assignedCoreIndex`, the current instance). Read them through a non-inlined
  accessor across switches. **Checked (session cloud, 2026-10-09): fibers do move, the build caches no address.**
  `WWHD_THREAD_STATS` now gives each guest thread's cores and moves: continue at 60 with three host threads
  (desktop worker), `update_ubo` (affinity cores 0 and 2) moved 19147 times in 365k timeslices and the Alarm
  Thread (any core) 1273 times; every other thread has one core. `tools/sixty/perf/tls_across_calls.py` reads
  the binary's disassembly for a thread pointer (`mov %fs:0x0,%reg`) or a pointer derived from it kept in a
  callee-saved register across a call and used after it: 1 of 79759 functions (write_watch's `ThreadInit`, which
  runs once on a host thread's own stack, no fiber switch in it); no generated function, none of Cemu's. The
  rest reach thread-locals as `%fs:offset` at each use (local-exec and initial-exec in the PIE executable),
  which reads the host thread they run on. Re-run the scanner after a compiler or flag change.
- The TCL ring (`TCL.cpp`) is single-producer: fine while only the GX2 main core flushes (it does; others are logged).
- The real-time metrics measure one host thread (`__OSIdleNanoseconds`, the frame log's `cpu_ms`): per core.

**Fine as is**: the scheduler lock (a futex with a `thread_local` count), Mutex/Event/ThreadQueue/MessageQueue,
alarms, the real-time timer, waking threads from host threads (`__OSAddReadyThreadToRunQueue` signals each core),
GX2's per-core command buffers, AX's voice-list spinlock.

## The prototype (session cloud, branch ww-4-cloud)

- **Three host threads without Cemu's JIT.** `REF_CPU_MODE=3` (Cemu's "multicore recompiler") crashed at once: that mode
  also turns on Cemu's own JIT, which runs guest code before our hook (an AX callback faulted in its interpreter).
  `--force-multicore-interpreter` (Cemu's launch flag: three host threads, JIT off; `REF_ARGS` in run.sh) leaves
  our hook running everything: the game boots and plays the whole continue route at 30 and 60.
- **The task loop's sleep with three host threads.** Without it the self-ticking task thread (OliveOperationMgrThread,
  core 1) took 74% of core 1's host thread. Now the quiet watch and the store journal's switches (`g_rtJournalOn`,
  `g_rtStoreAll`, `g_rtStorePages`, `g_rtStoreCensus`, `g_rtJournalThread`) are per host thread (`thread_local`), the
  fast paths are allowed with three host threads, and core 1's host thread sleeps when idle (`__OSIdleWait`, its own
  run queue).
- **The 60 fps per-step values per host thread** (`g_rtStep`, `g_rtNote`, `g_rtLateNotes`, `g_rtHold`, `g_rtTickWindow`,
  `g_rtActor`, `g_rtLinkGroundLost`, sixty.cpp's `s_drawing` and `s_rndExec`): set only on the frame thread's host
  thread, so the sound and job threads on cores 0 and 2 never see its step. `g_rtHalfTick` (the frame's) stays shared.
- **One host thread unchanged:** checks all MATCH on this build (traces, command streams, sound, diff 0 mismatches,
  captures PSNR inf).
- **A/B** (`worker-ab.sh`, continue, lazy DrawDone on in all, 5 rounds paired; every run separated):

| | one host thread | three host threads |
|---|---|---|
| fps at 60 | 47.3 | **53.8 (+13.4%)** |
| the game thread's CPU a frame at 60 | 20.6 ms | **15.3 ms (-25.6%)** |
| core 1's host thread busy at 60 | 97% | 82% (cores 0 and 2: 22% and 14%) |
| the render thread's fence wait at 60 | 0.2 ms | 2.6 ms |
| fps at 30 | 27.0 | 28.7 |

  With three host threads the game thread fits 60's budget on the worker; what's left is its Intel iGPU (the render thread
  now waits for its fences), weaker than the Deck's GPU. (the worker misses even 30 by a little: 27 fps with one host
  thread, its GPU and the 30 fps pacing; not looked into yet.)

**On by default in real time (session top, 2026-10-09, WW-13 t-cores3).** `OSSchedulerBegin` runs three host threads
unless `WWHD_CORES=1`; the virtual clock (every check) keeps one; the settings page's "CPU threads" defaults to 3. What
was left, done:
- *Locks.* FSA: served under a host mutex already (`coreinit_FS.cpp`). Counters: dispatch.cpp's native counters are
  relaxed atomics bumped with a load and a store (no locked add on the hot path; a lost count is fine for a report);
  `g_rtStoresPassed` is per host thread (the frame thread's, which its frame log reads). The rollback's copies: probe
  `WWHD_CORE_CENSUS=path` notes which guest threads run on the other host threads at the frame thread's copies (the
  half tick's rollback, the converted globals hidden and shown). tour3 at 60 on the desktop worker: of 1289 rollbacks,
  29 had core 0's sound threads running (JASThread 25, nw::snd::SoundThread 1, an OS thread 3), and none had anything
  on core 2: the job lists (RenderDisplay's) are joined before the frame ends. The copies only put back bytes the
  frame thread itself stored in that half frame, so a sound thread storing the same bytes at that moment is the lost
  update one host thread has too (a timeslice switch there), not a new hazard.
- *Per-core figures.* The frame log's `core0_ms` and `core2_ms` (cores 0 and 2's host threads' CPU over the frame;
  `worker_absum.py`'s core0/core2 %), and the real-time log line's "cores 0 and 2's threads N% and M%"
  (`__OSCoreCpuNanoseconds`). The scheduler thread's "busy" is core 1's (the main thread's) host thread.
- *Routes, three against one* (`rt_routes.sh`: predeploy's 52 routes in real time at 60 with the null GPU, twice each
  way, 4 at once on the desktop worker; Link's path compared pair by pair, runs that came into play late left out).
  Of the pairs that played on time, exact (0.0 units all the way): one against one 40 of 42, three against one 141 of
  154 (92%), three against three 32 of 35. 27 routes are exact in all six pairs. The rest: warp (both three-thread runs
  6.1 units from both one-thread runs at the end, 38 on the way), sail 7.3/9.0, gtgrapple 30-36/55-64 (one against one
  6.4/17.7 there), and en-ph's fight, apart in every pairing, one against one too (391-545 units: its random stream).
  So three host threads keep the routes as one does, within real time's own noise.
- *Speed* (`worker-ab.sh`, the worker, continue at 60, the lazy DrawDone, one binary, 4 rounds paired): 48.1 -> 53.5
  fps (+11.7%, every run above every run), the game thread's CPU a frame 20.2 -> 15.1 ms (-25.4%), core 1's host thread
  97% -> 81% busy, cores 0 and 2 21.5% and 14.5%; the render thread's fence wait 0.7 -> 3.8 ms (the iGPU is the limit).
- *A run that never reaches play* (found here, a harness effect): in real time the routes' first scripted A (the
  title, frame 420) can come before the title takes input on a busy machine. The run then sits on the file select
  (no process made after the title's), one A behind at each menu. It happened in about half of the three-thread runs
  6 at once on the busy desktop worker, and with one too (later on the desktop worker nearly every run, either way:
  not load alone, the cause not found). The same runs with the menu presses 150 frames later: 6 of 6 in play.
  `WWHD_INPUT_CALLERS` (a probe) showed the pad read only by the main thread, once a frame, every sample taken; the
  thread dumps (`WWHD_THREAD_DUMP`: each guest thread's state, wait objects and guest call chain at exit) were the
  same in a stuck run and a good one. `rt_routes.sh` now moves the first three menu presses later
  (`WWHD_INPUT_REMAP`: 420, 540, 660 -> 530, 620, 700; Start stays at 780, so play begins on the same frame) and still
  plays a late run again (`RT_RETRIES`, 3). A player presses A when the title is up, so play doesn't see it. Next
  (t-boot, the owner's ask): `WWHD_DEBUG_BOOT`, straight into a save's play without the title.
- *Soaks* (`soak.sh`: an hour of seeded random play on Outset at 60, rendered, the lazy DrawDone: walking, camera,
  sword, items, the pause menu): the worker 216,900 swaps in 73 min, 49.7 fps over the first 21 min (game thread 80%
  of a core, cores 0 and 2 26% and 14%); the desktop worker in 61 min at 59.8 fps (55%, 12%, 9%). No crash, panic or
  assert. Not the continue route: the 53.5-53.8 fps here are continue's gameplay window (worker-ab.sh).
- *Gates* (the default build, on main 1b4ef02): checks all MATCH (traces, command streams, sound, diff 0 mismatches, 15 captures PSNR
  inf); regress identical to main's (md5 da06a4ee); predeploy 50 ok, 0 FAIL, gohmatail WARN 54.0 (as on main), gohmarock ok 4.8.

(Session cloud's list of what was left before the default: route correctness, a soak, the locks list, a `WWHD_CORES`
switch, per-core figures in the real-time log: all done above. Still open: the owner's PC with a window.)
