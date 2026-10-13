# Uncapped: research and a plan (b-uncapped-plan, session bottom, 2026-10-11)

The owner's ask: frame rates beyond 60. The research and the plan; the owner approved it and building started (2026-10-11, the answers at the end). Session
top reviewed it (2026-10-11: the cost table, pacing, the frame skip, `HalfStep()`'s audit). Line numbers are as of main
f32dbfe.

## Where we are: D21's 60 is two steps a tick

At 60 every 30 Hz tick runs as two frames: a **whole** frame and a **half** frame (`g_rtHalfTick` by swap parity,
sixty.cpp `f_0274C264` :3254). Converted processes (401 in `kConvertedByDefault`) step `g_rtStep = 0.5` in both frames,
so their second step ends where 30's tick ends. Unconverted processes and whole-tick systems run in the whole frame
only. The half frame journals and rolls back what other code writes (`WWHD_60FPS_ROLLBACK`), and saves and restores
the random stream. D21 itself says uncapped is "the same machinery with a variable h and a 30 Hz phase accumulator"
(recompiler-design.md :1295). The question is how much of the machinery really is general in h. Measured from the
code (an Explore pass over sixty.cpp, sixty_step.cpp, generate.py and the 3,037 rules):

| part | general in h | assumes exactly two steps a tick |
|---|---|---|
| step math in the runtime (`rt_step_mul/div/approach/damp`, the arc term) | yes | `rt_step_split` (`s/2`, `s - s/2`) |
| rules, 3,037 | 912 (`*h`, `k`, `vec`, `fall`, `d`, `/h`, ...) | 411 with a literal half (`split` 174, `spliti` 165, `lagi` 18, `halfadd` 15, ...); 1,696 whole/late gating (`whole` 718, `keep` 364, `eqwhole` 306, `late` 297) that need a meaning for "late" with more than one extra frame |
| sixty_step.cpp's 49 overrides | the approaches and chases, `calcSpeed`/`posMove`, `decOldFrameMorfCounter`, the frame controls' rate | ~20: the integer splits (`ScaledInt`), "only one step a tick counts" (`HalfStep()`: timers, collider registration, hit clears), "the whole step's" references (`StepId() - 1`: Link's anim hold, TICKVEC centres, `posMoveFromFootPos`, `updateMonitor`), the drawn facing at the midpoint |
| sixty.cpp's frame | the converted process's execute | `g_rtStep = 0.5f` in four places (:3947, :1238, :4077, :4130), one rollback journal per half frame, `GameFrame = from + (swap - from) / 2`, the event edges cancelling "the half step" |
| checks | the state probe itself | probe keys `tick*2(+1)`, predeploy's 2k±1 matching, compare.py's midpoint test, hz30's parity, the trial's 0.5 |
| pacing | keep-speed's tick schedule (33,333 µs) | frames paired (`s_pairVsync + 2`); the 40/50 caps as a `tick % 3` drop pattern of half frames |

Two facts carry the plan. First, **integer splits telescope for any whole number of equal steps**: a step `s`
split N ways (`s/N` rounded the same way each time) and a divisor `d` stepped as `Nd-N+1 ... Nd` both end exactly
where 30's single step ends. They don't have an exact form for an arbitrary fraction. Second, `fopAcM_posMove`'s
`p += h·v + (1-h)/2·Δv` sums to 30's tick for **any** steps that add to 1. Most of the float math is like that; the
integer parts and "which frame is the tick's" are not.

## The three ways

**A. True variable step (any h, a 30 Hz phase accumulator).** Each frame steps by the real time since the last, and
the tick ends when the steps reach 1. Pros: any refresh rate natively, VRR friendly. Cons: the 411 literal-half rules
and ~20 overrides have no exact fractional form (integer timers, `(int)getFrame() == 15` tests: 185 sites, per-tick
random tests: 151 sites, every-Nth-tick masks: 87, all need a tick boundary, which a variable step crosses at an
arbitrary point); the last step of a tick must land exactly on 1, so steps get clipped, and the frames around a tick
boundary differ in length; and **nothing is checkable**: today's checks rely on fixed sub-steps whose last equals 30's
tick, which holds only when the steps are fixed. The rival's "True 60" (rival-study §5) is a rolled-back dt preview
and covers locomotion only; it doesn't solve this either.

**B. A fixed higher tick rate: N equal steps a tick (h = 1/N).** 60 = N 2, 90 = N 3, 120 = N 4, 150 = N 5, 240 = N 8.
Everything D21 built generalises with exact arithmetic: splits telescope, "whole" is step 0, "late" is step N-1, the
last step ends on 30's tick, and the checks keep their invariant (the last sub-step equals 30's tick k; sub-step i lies
between ticks k-1 and k). Cons: each extra frame costs the half frame's path again (the game thread +73-83% at 60,
deck-plan.md :18; the half step's executes ~13% of that, the rest the frame path and the journal), and a display
whose rate isn't a multiple of 30 needs pacing (below).

**C. Hybrids.**
- *B plus frame generation:* N steps of real logic to 120, FSR 3 frame generation (b-framegen, landed) doubles it to
  240 for displays that fast. Frame generation is opt-in and adds a frame of latency; that's the owner's trade.
- *B plus presentation pacing:* for 144/165 Hz, run N 4 (120) or N 5 (150) and either present with VRR (the frame
  shown when ready, WWHD_VRR) or keep vsync with a fixed drop pattern, as 40/50 drop half frames today (`tick % 3`).
- *B plus interpolation:* the rival's approach, logic at a lower N and the picture blended between steps (camera,
  J3D matrices, particles). Not recommended: D21 rejected interpolation for the 60 (latency and the extra
  bookkeeping per system), and with B's exact steps it would add nothing that frame generation doesn't do cheaper.

**Recommendation: B, with C's pacing and frame generation on top.** It is the only option that stays verifiable
with today's tools. It reuses all of D21. At N 2 it must be **byte-identical to today**, which is a gate in itself,
and 60 can't regress. Variable step (A) is not worth its cost: VRR plus N 4/5 covers 120-165 Hz screens, and frame
generation covers 240.

## Verification at N steps

- **N 2 identity:** with the generalised runtime at N 2, checks all MATCH, regress unchanged (da06a4ee), predeploy's
  30-against-60 unchanged to the unit. This proves the refactor before any new rate exists.
- **The tick invariant at N:** the probe keys `tick*N + i`; predeploy and regress compare 30's tick k with sub-step
  `N(k+1)-1` (the tick's last), within today's tolerances (FAIL 150 units, WARN 40), and check that sub-step i is
  within the box of ticks k-1 and k (compare.py's midpoint test, generalised to "between").
- **Every N, not just 4:** the routes at N 2, 3 and 4 (3 is odd: no step is "the middle", which flushes out code
  that still means "the half" by "not the whole"). The step-doubling trial (`WWHD_60FPS_TRIAL`) runs N steps of 1/N
  at 30 and diffs the stores against one step, per converted process.
- **Pictures:** `hz30.py` generalised from parity to period N (a block that changes on one sub-step of N is a
  system left whole). `judder.py` against even spacing of the N frames.
- **Real time:** predeploy-rt and the soak at each rate, on both workers.

## Presentation

- The swap interval: today 1 at 60 (`wwhd_SetSwapInterval(1)`). At N with a display of exactly 30·N Hz, the same;
  pacing pairs frames today (`s_pairVsync + 2`), which becomes N-tuples (`+ N`).
- Displays that aren't a multiple: **VRR** first (present each frame when ready; the keep-speed schedule still
  places ticks every 33,333 µs, and frames inside a tick at i/N of it). Without VRR, a fixed drop pattern like 40/50's:
  e.g. 144 Hz from N 5 (150) shows 24 frames in 25. A drop keeps the logic (dropped frames' logic runs, their draws
  don't), exactly as `WWHD_FPS_CAP` does.
- **Frame rate setting:** 30, 40, 50, 60, 90, 120 and "the display's" (N picked from the refresh rate, VRR when
  available). Frame generation stays its own setting; with it, "the display's" can pick N for half the refresh rate.
- **Keep-speed and frame skip** generalise directly: a sub-frame that would end after the next tick is due is dropped
  (its logic kept, as the 40/50 caps do), and t-skipcost's 0xFC dropped-frame marker works as it is; below 30, whole
  frames as today (`g_dropFrame`, which at 30 suppresses the swap). 30's own pacing slot (`Paced30`, `s_slot30 + 2`)
  and 60's pairing (`s_pairVsync + 2`) both become `+ N`.
- **The cost limits the rate** (top's arithmetic, from the review): each extra step costs about one half frame of
  game-thread time. On the worker at 60 the game thread takes 12.4 ms a frame (wwhd-play, continue; 13.4 for
  wwhd-null, 11.1 with PGO), so a tick costs 2 x 12.4 = 24.8 ms of its 33.3 ms. With the half frame ~0.78 of the whole
  (60's +73-83%), that is a whole of ~13.9 ms and a half of ~10.9 ms. Per 33.3 ms tick:

  | N (rate) | game thread a tick | with PGO | of the budget |
  |---|---|---|---|
  | 2 (60) | 24.8 ms | 22.2 ms | 74% (67%) |
  | 3 (90) | 13.9 + 2 x 10.9 = 35.7 ms | ~31.8 ms | 107% (95%): barely fits with PGO |
  | 4 (120) | 13.9 + 3 x 10.9 = 46.6 ms | ~41.5 ms | 140% (125%): doesn't fit |

  So on the worker's class of CPU, 120 is out unless the half frame itself gets cheaper, and 90 is marginal. The
  half frame's fixed costs (the frame path around the executes, the rollback journal and its restore) are the target,
  not the converted executes (~13% of 60's extra). A desktop CPU with more single-thread speed shifts the table; U0
  measures it on the desktop worker too. The Deck stays at 60 (or 40/50) regardless.

## U0: the table measured (session bottom, 2026-10-11)

`worker-ab.sh` 30 against 60 (and 60 with `WWHD_60FPS_ROLLBACK=0`, the journal off: a probe, not playable), wwhd-play,
4 rounds; each 60 run's frames split by `frames.py` into whole and half (median / 90th percentile of the frame's work,
from swap 1000); the profile (`WWHD_PROFILE`, its half-frame samples tagged since U0: `profile_report.py --half`).

**Where a half frame goes** (desktop, continue, the game thread's samples in half frames): the processes' draws
(fpcM_Draw) 31%, RenderDisplay::draw (the render lists) 25%, the converted executes 23%, the frame's other parts 18%,
the journal's own functions 3%. But the journal cost more than its functions: with it off a half frame took 4.7 ms
against 6.4, and RT_STORE's filter alone (on, with nothing kept: a throwaway probe) 6.2. The filter on every store
was the cost, mostly clang's test of each `thread_local` flag's TLS init function. **Cut 1** (ff9505b, landed): the
flags `constinit`, the half frame 6.4 -> 5.3 ms and the whole 5.4 -> 5.0 on continue (soak: the game thread 35% ->
31% of a core).

After cut 1, a frame's work (ms):

| machine, route | 30: a frame | 60: whole | 60: half | half, journal off | late at 60 |
|---|---|---|---|---|---|
| desktop, continue | 5.2 | 5.0 / 5.6 | 5.3 / 6.0 | (4.7 before cut 1) | 0% |
| desktop, en-tn | 6.2 | 4.9 / 6.7 | 5.1 / 7.3 | | 0-1% |
| speed worker, continue | 17.0 | 11.5 / 16.9 | 12.2 / 16.9 | 10.7 / 16.9 | 14-23% |
| speed worker, en-tn | 16.9 | 13.8 / 18.3 | 14.9 / 18.3 | 14.9 / 18.0 | 53-56% (53 fps) |

So the half frame costs about what the whole does, not 0.78 of it (the draws are a frame's, whatever its step), and
**each frame has its own vsync's budget**: a tick's total isn't enough, every frame must fit 1/rate. Per rate:

- **Desktop class (the desktop worker):** 90 (11.1 ms) and 120 (8.3 ms) fit with room, even at the 90th percentile;
  144 (6.94 ms) fits at the median but not en-tn's 90th percentile (7.3); 165 (6.06 ms) fits neither 90th percentile,
  so 144/165 run with keep-speed's drops or VRR below the refresh (below). The game thread at 120 is ~5.0 + 3 x 5.3 = 21 ms a tick (63% of a core).
- **The speed worker** (iGPU and CPU under one power cap) is at its limit at 60 already (late 14-56%), and the Deck
  drops at 60 too: 90 and 120 are not for that class. Its half frame isn't the issue (journal off saves 1.5 ms, 12%);
  the frame is.
- **Rates offered:** 90, 120 and "the display's" as settings, for machines that run them; keep-speed drops what a
  machine can't (as at 60 today). The U0 gate's "half frame ≤ 6.5 ms on the worker" doesn't hold and can't by cutting
  the half frame alone: U1-U5 go ahead, verified at 90/120 on the desktop worker; the speed worker and the Deck stay
  at 60 (or 40/50).
- **Left in the half frame:** the journal's remaining filter (~0.6 ms on the desktop, ~1.5 on the speed worker).
  **Cut 2 tried and dropped** (session bottom, 2026-10-11): RT_STORE's page table as a 128 KB bit per page instead of
  the 4 MB table (so a frame's heap stores would stop missing the cache) was correct (checks MATCH, regress da06a4ee)
  but bought nothing: on the speed worker, 4 rounds paired, continue's half frame 13.0-13.2 -> 13.1-13.6 ms and en-tn's
  game-thread CPU +2.8% (higher in every run). The filter's cost is the per-store inline test itself (a TLS load and
  branches in every store of the generated code), not the table's misses.
- **So, with cut 1 only, on the desktop class:** 144 Hz (6.94 ms a frame) fits continue at its 90th percentile (half
  frame 6.0 ms) but en-tn only at the median (5.1 ms): its 90th percentile (7.3 ms) is over, so on heavy scenes about
  one frame in ten misses 144 and keep-speed drops it (or VRR shows it late). 165 (6.06 ms) fits both routes' medians
  and neither 90th percentile (continue 6.0 is at the edge): it runs with VRR, or with drop patterns, below the refresh.
- **Queued (u8-journal; the owner's go, 2026-10-11):** a steady 165, and 144 without drops on heavy scenes, need the
  store journal redesigned so that a store carries no inline test: e.g. the half frame's stores journaled by
  write-protecting the journaled pages (a fault on the first store to each) or by a separately generated half-frame
  copy of the code. A large job (the generator and the runtime), in session bottom's lane after U5 and b-lsfg. Its
  target: en-tn's half frame at the 90th percentile under 6.06 ms on the desktop worker, N 2 byte-identical.
- **u8's census (session bottom, 2026-10-12; a store census probe, not landed: the virtual clock at 60, per half
  frame):** what a test-free journal would have to keep.

  | route | stores | near the stack | into journaled pages | live | hand-written | journaled pages written | other threads' into them | journaled bytes changed unsaved |
  |---|---|---|---|---|---|---|---|---|
  | en-tn | 1,035,000 | 72% | 40,100 (3.9%) | 2,500 | 1,250 | 43 (of ~850 journaled) | 3,560 | 856 |
  | sail | 775,000 | 68% | 33,900 (4.4%) | 1,800 | 1,200 | 42 | 2,780 | 551 |

  **Write-protecting the journaled pages** (a fault on a page's first store, a copy, the page put back at the frame's
  end) would take ~43 faults a frame, but it doesn't keep today's meaning. Other threads write those pages (3,600
  stores a frame: in real time from other host threads), and live stores, the OS layer's and >32-byte stores change
  ~850 journaled bytes a frame that the journal leaves alone. A page copy put back would undo them all. Byte-exact
  versions (single-stepping a foreign fault, or diffing pages against a second copy) cost more than they save.
- **Where the journal's 1.5 ms goes (the desktop worker, wwhd-play, en-tn:1800, 4 rounds paired; a frame's work,
  median / 90th percentile, ms; probes on session bottom's work branch):**

  | variant | whole | half |
  |---|---|---|
  | as now | 6.5-6.7 | 7.3-7.5 / 8.0-8.3 |
  | the journal kept but not put back (a probe) | 6.6-6.7 | 7.4-7.5 |
  | the journal off (`WWHD_60FPS_ROLLBACK=0`, a probe) | 6.5-6.6 | 5.8-5.9 / 6.0-6.4 |

  Not putting the journal back costs nothing less: the rollback makes the next frame redo no work. A timing probe
  puts the frame's fixed parts at 34 us (the ~850 pages' table and bits) and 2.4 us (the rollback), with ~21,000
  calls into rt_journal_store and ~380 saves a half frame. So the cost is the inline test on the ~1M stores of a half
  frame: each jumped out of line to read three TLS switches and a global, look up two words of the 4 MB page table
  and jump back. An earlier probe that let the r1-based 70% of the stores skip the lookups saved only 0.2 ms: the switches were
  still read.
- **Cut 3 (u8, landed):** RT_STORE's test is one TLS pointer, `g_rtStoreTable`: a byte per 4 KB page, null while
  the journal is off, every byte set for a journal that takes every store (a diff run's, a quiet watch's), else the
  half tick's pages (`g_rtStorePageBytes`, kept beside its word table). The runtime sets it from the switches whenever
  one changes (diff.cpp `rt_store_table_update`). A store under the journal is one TLS load, two byte loads and a
  branch. Byte-identical by construction (checks MATCH, regress da06a4ee, statetest 0 differ). The desktop worker,
  en-tn:1800, 4 rounds paired: the half frame 7.3 / 8.0-8.3 -> 6.8-6.9 / 7.6-7.9 ms, the work a frame -4.3% (lower
  in every run). About 0.45 of the journal's 1.5 ms; the half frame's 90th percentile is still over 165's 6.06.
- **u8 closed at cut 3 (2026-10-13).** Left: ~0.3 ms of calls (~21,000 a half frame, ~380 saves) and ~0.5 ms of the test
  itself. (b), a finer table (a byte per 64-byte line), measured first (a line census probe, the desktop worker, real
  time, 1,800 half frames): of the calls passed over as saved, en-tn 14,352 a half frame, only 768 (5%) are into a
  64-byte line wholly saved (256-byte: 115); continue 12,516 -> 625 / 58. Saved and unsaved kept bytes interleave
  within lines, so it would save ~0.015 ms. Saving whole lines on a first store would make them so, but the rollback
  would then put back other threads' and converted processes' stores into those bytes: not byte-exact. The test
  needs (a), the half-frame code as a second generated copy, dropped as too large for its gain.
- **U5's first rates (session bottom, 2026-10-11):** from the 60 fps switch the vsync runs at 30 N, in real time and
  under the virtual clock, so a frame a vsync is N frames a tick. The Frame rate setting offers 90, 120, 150 and "the
  display's": the most frames a tick its refresh shows, up to 5 (144 Hz 120, 165 Hz 150; for VRR displays), or half
  that with frame generation on (240 Hz: 120 shown as 240). Frame generation's check uses 30 N. Soaks on the quiet desktop
  worker (3 minutes' route, WWHD_CORES=3): 60 at 60.1 fps (0.2% late), 90 at 90.1 (0.6%, game thread 53%), 120 at
  120.0 (4.2%: keep-speed drops 1 frame in 24; game thread 70%).
- **The Deck:** QA measured the Deck OLED at 36 fps on en-tn and ~50 at the start of continue at 60 (2026-10-11), so
  the OLED's 90 try is off; it's revisited when the Deck holds 60.

## U2: the rules at N (session top, 2026-10-11)

`split`/`ssplit`/`spliti` were N-way already (U1's `rt_step_split`), and `keep` and `eq/gt/ltwhole` already read "not
step 0" (`g_rtHalfTick` is every step but the first). U2 adds the rest, each identical at N 2 (checks 15 captures PSNR
inf, regress da06a4ee):
- the lags (ppc_ops.h `rt_step_lag`, `rt_step_lagi`, `rt_step_drawlag`): a count kept to whole ticks, seen on step i,
  is (N - 1 - i)/N of a count ahead of the steps: `lag`, `lagi`, `lagw` and `drawlagi` take that back (1/2 on step 0
  at N 2, 0 on the last), `lagdown` gives it; `drawlag` (a count the whole tick's draw already moved on, in a later
  step's draw) takes (N - i)/N back;
- `halfadd`'s 15 rules by kind (`halfadd:N/on|/back|/key`, tick_rules/mtimer.txt): the draws' flash and chain angles
  i/N of a tick on, the Hero's Charm's bob and turn (an execute) (N - i)/N back, Zunari's flags' cloth keys one per
  step (with one key for steps 1..N-1 the cloths would move on one step of N); the ship's 0x1FF test a plain constant;
- the step-doubling trial at N steps (`WWHD_STEPS=N` at 30: N steps of 1/N, the draw between them with
  `WWHD_60FPS_TRIAL_DRAW`), and `tools/sixty/tests/trial_n.sh ROUTE FROM TO [N]`: the trial of every converted process
  at 2 and at N, and the instructions that differ from the game's step at N only.

The audit (after U3; tour3, sail, warp, hook, bombs and cuts, 200-400 ticks each): of 1,100-1,250 instructions that
differ at 2 steps (today's, open-coded or rounding), 30-85 a route differ at 3 only, and none at a rule: the clouds'
band wrapping on another step (positions a period apart), float rounding of large coordinates (half a unit on 300,000)
and of 1/3 steps (an animation's frame at 40.0 of a 40-frame loop instead of 0), matrices and other heap scratch, and
two override sites (session bottom's: the pots' `MoveC` settling, and the camera, whose steps before the last are for
show and which the trial compares without the camera tick's restore). At N 4 (tour3) 6 more than at 3, each on 3 ticks: heap
bookkeeping and an NPC's flag byte.

## What doesn't go to N steps (and stays on whole ticks)

The whole-tick systems stay whole: process lifecycle and counters, input, the HD UI's update and the game manager (the
pause menu closed a tick early when run every frame), collision resolution (moved to the tick's last step, as
`ResolvePending` does for the half), collider registration, events advancing cuts. Cutscenes keep their D21 status
(converted puppets only; JStudio's frames are whole ticks). New per-system issues at N > 2: the wake's strip emitter
(`calcCreatePtcls`, every frame at step 1: twice the particles at 60, four times at 120, and its room of 150 segments
holds 80 today, so 160 at N 4 overflows: emit per tick there, or scale the room); sound requests (served up to a step
sooner; per-effect timing still needs the `mDoAud_seStart` hook, D21's open item); the UI animations' half step
(`UiAnimsHalfStep`) at 1/N.

## Phased tasks (complexity on plan.json's 1-10 scale)

| # | task | complexity | gate |
|---|---|---|---|
| U0 | **Measure the cost of a step, and cut it.** Profile the half frame on both workers (the frame path, the journal, the converted executes): the per-step cost and the table above measured, not estimated. Then the cuts to the half frame's fixed costs (the journal's restore, the frame path's parts that needn't run per step); without them 120 doesn't fit the worker (above). Decide the rates offered. | 5 | the table measured; for 120 a half frame of at most ~6.5 ms on the worker (~7 with PGO): 13.9 + 3h ≤ 33.3 |
| U1 | **The runtime at N.** `g_rtSubStep` (0..N-1) and `g_rtSteps` replace the parity; `RT_WHOLE_TICK()` = step 0, `RT_LATE_TICK()` = the last step (or the whole step for an unstepped process), `rt_step_split` N-way, the four `0.5f` become `1.0f / N`, `GameFrame` `/ N`, one rollback journal per non-whole frame, the event edges cancel the remaining steps. `WWHD_STEPS=N` (2 by default when 60 is on). wwhd-play's `RT_LOOP` and the loose build don't depend on N. Frame skip drops sub-frames and keeps their logic. | 7 | N 2 byte-identical: checks MATCH, regress da06a4ee, predeploy unchanged |
| U2 | **The rules at N.** The generator emits `split`/`ssplit`/`spliti` as N-way, `lagi`/`lagw`/`drawlagi` as `IMM·(N-1-i)/N` lags, the `±0.5` lags as `(N-1)/N`-based; `halfadd` re-derived (15 rules, by hand); `keep` and `eq/gt/ltwhole` read "not step 0". An audit pass over the 411 literal-half rules with the trial at N 3. | 8 | N 2 identical; the trial at N 3 and 4 clean per converted process |
| U3 | **The overrides at N.** `ScaledInt`'s telescoping for N; each `HalfStep()` user audited for what it means: "count once a tick" wants step N-1 only (or step 0 only), not "every step but the first", or a timer advances N-1 times a tick (top's review); `StepId() - 1` → the tick's first step; the drawn facing at `i/N`; Link's anim hold and ExactFrame, TICKVEC centres, `posMoveFromFootPos`, `updateMonitor`, the ship and the water holds. | 8 | as U2, plus predeploy at N 3 and 4 |
| U4 | **The checks at N.** Probe keys `tick*N+i`, run.sh `SIXTY_STEPS`, compare.py "between", predeploy's last-step matching and its runs at N 2/3/4, regress at N 4 for the converted enemies, hz30 by period, judder by spacing. | 6 | the tools at N 2 report as today |
| U5 | **Presentation.** Pacing by N-tuples, the drop patterns for 144/165 (no VRR), keep-speed at N, the frame rate setting (90, 120, the display's), frame generation picking N for half the refresh rate, the frame log's marks. | 6 | real-time soaks at 90/120 with and without VRR on the desktop worker; predeploy-rt per rate |
| U6 | **Per-system fixes at N > 2.** The wake's emitter, the UI animations at 1/N, sound request timing (the `mDoAud_seStart` hook), whatever U2-U4 turn up. | 5 | the captures and hz30 at N 4 clean |
| U7 | (only if the owner wants it after U5) **A variable step.** On top of N steps, not instead: N picked per tick from the measured frame time (N changes only at tick boundaries, so every tick is still exact and checkable). | 6 | the same gates at mixed N |

U0 first: by the arithmetic above 120 needs the half frame cut by about 40% on the worker, and 90 fits only with PGO;
its numbers decide which rates U1-U5 are worth
building. U1 is the critical one: N 2 byte-identical keeps everything D21 proved. U2 and U3 can run in parallel
(top and bottom), once U1 lands. U7 is the honest form of "uncapped": a variable rate made of exact ticks.

## The owner's answers (2026-10-11): approved, started

- **Rates:** all of them: 90, 120, 144/165 (VRR or drop patterns), and 240 by frame generation over 120.
- **A per-tick N (U7):** "whatever works best": parked, decided once U5's fixed rates run.
- **The Deck:** try 90 on the OLED after U0's cuts.
- **Who:** session bottom U0 (`u0-cost`), U1 (`u1-runtime`), U3 (`u3-overrides`), U5 (`u5-present`); session top U2
  (`u2-rules`), U4 (`u4-checks`), U6 (`u6-fixes`, with the Deck OLED 90 try). U1's gate: N 2 byte-identical.

## U9: the draw side, the main core's profile (session bottom, 2026-10-12)

The desktop worker, real time, en-tn:1800, `WWHD_CORES=3`, `WWHD_PROFILE_DEPTH=64`, reported for the game's main core
only (`tools/profile_report.py --thread core=1`; `--under f_XXXXXXXX` keeps a subtree, as shares of the core's samples).
The virtual clock's profile on the worker put 12% in the message queues; on the main core in real time it's 0.9%: that
was the other threads and the virtual clock's waits. The main core, the whole frame:

| subtree | share | its biggest self |
|---|---|---|
| `fpcM_Draw` (`f_025DF908`) | 25% | flat: no function over 1.2% (`f_028E8F64`, `psq_load`, `f_02555E0C`, `f_0246C5E8`) |
| `RenderDisplay::draw` (`f_0272A8C4`) | 21% | `f_02759564` 2.5%: a wait for the render jobs' counters (the other cores' `job_RunBatch`), not work |
| a loader thread's decompressor (`f_0275EBC0`, under `Run`) | 4.7% | not the frame's: a byte loop of Yaz0-style groups, on core 1 beside the game |

Below the two draws the profile is flat guest code (85% of the core), so the cuts are the emitter's, for every function
at once. The first: `psq_l`/`psq_st` with a float GQR type (0-3, where Cemu's `dequantize`/`quantize` copy the bits and
ignore the scale) are two word accesses inline (`ppc_ops.h`); the quantized types (4-7) stay out of line. The two
helpers were 3.7% of the main core. A/B, 4 rounds paired: the work a frame 6.22 -> 6.03 ms (-3.5%, lower in every run),
the whole frame's median / p90 6.25 / 7.0 -> 6.05 / 6.9 ms, the half frame's 6.5 / 7.3 -> 6.35 / 7.0 ms.

The second: the recompiled program reads guest memory through a constant view of `memory_base`. The generated code is
built with `-fno-strict-aliasing`, so every guest store may alias any global and the next access reloads the base:
in `f_02759564` 50 of its ~56 accesses dereferenced it first. `ppc_ops.h` declares `extern uint8* const rt_memBase`
for the shards only (`-DWWHD_RT_GENERATED`, src/CMakeLists.txt), bound by an asm label to a plain variable that
`dispatch.cpp`'s `BuildDirectTable` sets from `memory_base` in `Init`, which `Execute` runs once (`std::call_once`)
before any guest code. Cemu assigns `memory_base` once (`memory_init`'s 4 GB reservation, `if (!memory_base)`); state
loads restore memory, not the base. The overrides and the runtime keep `memory_base`. After it, `f_02759564`
dereferences the base 18 times, `f_02010FFC` names it once instead of 49 times, and wwhd-play is 2.3 MB smaller. The
label is a plain symbol (no mangling, the same on aarch64 and Mach-O; -fPIC reads it through the GOT, still invariant),
and the shards are built without LTO, so the constant declaration never meets the variable's definition.
A/B on the desktop worker (en-tn:1800, 4 rounds paired, `WWHD_CORES=3`): the work a frame 6.03 -> 5.94 ms (-1.5%,
lower in every round, every run below every run). The speed worker's A/B was within its noise (cpu -1.2%, overlapping).

After it, the main core's profile (en-tn:1800, the desktop worker): guest code 87%, the biggest single function the game's
Yaz0 decompressor (f_0275EBC0, 4.4%), run by a loader thread that shares core 1's host thread with the game; then
f_02759564 2.4%, f_028E8F64 2.2%, `pthread_cond_broadcast` 2.0% and a flat tail. The broadcast is Cemu's
`CounterSemaphore::increment` in `__OSAddReadyThreadToRunQueue`: real wake-ups of cores 0 and 2's host threads when a
thread they may run becomes ready, so not a cut. libc's 3.3% has no callers the stack walk can see.

Parked: the decompressor in host code (an override, real time only; the commit is kept on session bottom's branch
u9-yaz0-parked). The game's code step for step with the guest registers in locals, the same stores with the same guest
instructions for the journal, dcbz's lines, and yields at the same two loop heads after the game's instruction counts
(every register in ctx first, so a save state there resumes by interpreting). Its check mode (both on every call, the
output, the zeroed lines and every register compared) found 0 differences on warp, en-tn and tour3. Measured on the
desktop worker (on against off in one binary, 3 rounds paired): late frames not worse (warp 9/9/9 -> 9/7/8, en-tn
6/6/6 -> 5/5/6, tour3 6/6/6 -> 5/6/5), p99 the same, the work a frame flat (warp -0.6%, overlapping); the decompressor
4.4% -> 3.2% of core 1 (the byte loop is memory-bound). Its time was never on the frame's path, so ~1.2% of core 1
during loads doesn't pay for another override to keep; to come back if a device profile shows loads hurting (Android).
