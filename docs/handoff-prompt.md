# Prompt for the next agent (2026-10-01, after WW-3)

Paste the text below into a new Claude Code session started in the ww-4 worktree. If it doesn't
exist yet, create it from the main checkout with
`git worktree add -b ww-4 /srv/projects/WWHDRecomp/.worktrees/ww-4 ww-3`.

---

You're continuing the static recompilation of The Legend of Zelda: The Wind Waker HD (WWHDRecomp).
Work in the git worktree `/srv/projects/WWHDRecomp/.worktrees/ww-4` on branch `ww-4`, which starts
from `ww-3`.

Start by reading, in this order:
1. `docs/handoff.md`: the current state, the infrastructure (worker and the owner's desktop), which
   checks to run for which change, and "The next task", which is yours.
2. `CLAUDE.md`.
3. `docs/recompiler-design.md`: D21 first (60 fps: what the game does each frame), then D9
   (overrides), D19 (the scheduler: deterministic and real time) and D18 (what is ours).

**Your task: native 60 fps, then an uncapped frame rate.**
- The owner wants the game's own logic to run natively at 60 ticks a second and play exactly as it
  does at 30: the same speeds, jump arcs, timers, animations, cutscenes and sound sync. In the end
  it should run at any frame rate, uncapped, with a variable time step.
- **Not interpolation.** The owner ruled it out as the goal: no extra frames drawn between 30 Hz
  ticks. They want a solution of our own, and a recompilation can do things an emulator patch
  can't: the generator sees every constant, every field access and every call site.
- What's known so far is in D21 and the handoff:
  - the frame loop (sead's framework; `game_procFrameBody` is the tick, then the draw);
  - play is frame-locked while the scenery follows the clock;
  - presenting every vsync already works (`WWHD_60FPS=1`), with the game at double speed;
  - the GameCube version's 60 fps hacks slow it down with one global value and get physics wrong.
- Work in this order, and check with the owner at each step:
  1. **Research.** Find WWHD's time base and how its actor, camera, animation and particle code
     advances per tick. Use `zeldaret/tww` (the GameCube decomp) for names and structures, and the
     Ghidra project and `WWHD_BACKTRACE` to find them in WWHD. Write what you find into D21. Then
     propose an approach to the owner, as a numbered list of options with a recommendation, before
     building it.
  2. **A measuring tool.** Run the game deterministically at 30 and at 60 ticks a second (the
     virtual clock with `WWHD_VSYNC_HZ=120`, or swap interval 1), and compare game state at equal
     game times: tick 2N at 60 against tick N at 30, in guest memory, captures and OS calls. That
     turns "what breaks" into a list.
  3. **Fix what diverges,** behind a switch, until the 60-tick run matches the 30-tick run at
     equal game times. Scenery driven by the clock is the exception. The owner judges the feel on
     their desktop in a window.
  4. **Then uncapped:** a variable time step, presentation without vsync, and frame pacing.
- Done for 60 fps when:
  - state and captures at equal game times match the 30-tick run;
  - the owner is happy with how it plays;
  - with the switch off, every existing check is unchanged: `stream_check.sh` on the routes, diff
    mode, G3.

Rules to keep (details in the handoff):
- **Never commit or emit game data.** No ROM, RPX, extracted assets, decompiler output (Ghidra
  dumps stay in `/wwhd/data/ghidra-out` on the worker), generated recompiler output, shader caches,
  captures or saves. `build/` and `orig/` stay gitignored.
- **Heavy work runs on the worker worker, never on the editing machine.** That means Cemu, builds, Ghidra
  and traces. Use `tools/worker/sync.sh`, `tools/worker/w CMD` and
  `tools/worker/job start|wait|tail|stop NAME`. Never kill by pattern.
- **Wait in chunks.** Wait on jobs for at most 9 minutes per call, and tell the owner what's
  running. No silent long waits.
- **Real-time tests run headless on the owner's desktop:** `ssh owner@DESKTOP_ADDR`, in
  `~/wwhd-test`, with `WWHD_WINDOW=0` (silent). `~/wwhd-play` is the owner's: don't touch their saves
  or cache, and open a window there only when they ask. The desktop may be asleep (offline), and the
  owner may be using it: keep runs short.
- **Commits:** `git -c user.name="flossbud" -c user.email="224492734+flossbud@users.noreply.github.com" commit`, with the
  trailer `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`, then `git push worker ww-4`.
  Commit each step once its checks pass.
- **Every rename in `config/US_v0/symbols.csv` needs evidence.**
- **Keep a future Android release in mind:** portable code, Vulkan features Android drivers have,
  low CPU use. 60 ticks a second doubles the game's CPU work, and phones throttle.
- **The owner often reads on a phone.** Give short, plain progress updates. For a decision, use the
  question tool, with the options listed and one recommended.
- **Keep the docs current:** update `docs/recompiler-design.md` (D21, and D9 for new overrides) and
  `docs/handoff.md` as you finish things.
