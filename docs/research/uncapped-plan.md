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
