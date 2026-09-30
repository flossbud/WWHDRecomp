# Prompt for the next agent (2026-09-30)

Paste the text below into a new Claude Code session started in the ww02 worktree.

---

You're continuing the static recompilation of The Legend of Zelda: The Wind Waker HD (WWHDRecomp).
Work in the git worktree `/srv/projects/WWHDRecomp/.worktrees/ww02` on branch `ww02`.

Start by reading, in this order:
1. `docs/handoff.md`: the current state, the infrastructure (worker and the owner's desktop), which
   checks to run for which change, and the plan for your tasks.
2. `CLAUDE.md`.
3. `docs/recompiler-design.md`, sections D9, D18, D19 and D20.

Your tasks, in order:

1. **Let the game's task switcher sleep in real time** (handoff, "Next steps" 1).
   - First build the D9 override mechanism: a generated function replaced by our own, with the
     original still callable as `orig_f_X`. Nothing of it exists yet.
   - Then override the task switcher (`f_02760ACC` → `f_0275FFCC`). In real time only, it should
     block when no task is ready, instead of spinning through `OSSendMessage` and
     `OSReceiveMessage`.
   - The deterministic (virtual-clock) mode must never take the new path.
   - Done when:
     - real-time runs on the owner's desktop show the scheduler thread well under 100% busy;
     - 30 fps and the ~35 ms 99th-percentile frame time are unchanged;
     - the sound is intact (the owner listens);
     - both routes' `stream_check.sh` are identical and diff mode is clean.
2. **Fewer Vulkan pipelines through dynamic state** (handoff, "Next steps" 2).
   - Stencil reference and masks first (core Vulkan 1.0), then extended dynamic state 1 and 2
     (core in 1.3), and extended dynamic state 3 only with a fallback.
   - Done when:
     - pipeline counts are down (report before and after);
     - captures are byte-identical to captures made before the change;
     - `tools/reference/shader_cache_check.sh` passes.
3. **Then continue down the handoff's list**, asking the owner which item comes next.

Rules to keep (details in the handoff):
- **Never commit or emit game data.** No ROM, RPX, extracted assets, decompiler dumps, generated
  recompiler output, shader caches, captures or saves; `build/` and `orig/` stay gitignored.
- **Heavy work runs on the worker worker, never on the editing machine.** That means Cemu, builds, Ghidra
  and traces. Use `tools/worker/sync.sh`, `tools/worker/w CMD` and
  `tools/worker/job start|wait|tail|stop NAME`. Never kill by pattern.
- **Wait in chunks.** Wait on jobs for at most 9 minutes per call, and tell the owner what's
  running. No silent long waits.
- **Real-time tests run headless on the owner's desktop:** `ssh owner@DESKTOP_ADDR`, in
  `~/wwhd-test`, with `WWHD_WINDOW=0` (silent). `~/wwhd-play` is the owner's: don't touch their
  saves or cache, and open a window there only when they ask.
- **Commits:** `git -c user.name="flossbud" -c user.email="224492734+flossbud@users.noreply.github.com" commit`, with the
  trailer `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`, then
  `git push worker ww02`. Commit each step once its checks pass.
- **Every rename in `config/US_v0/symbols.csv` needs evidence.**
- **Keep a future Android release in mind:** portable code, Vulkan features Android drivers have,
  low CPU use.
- **The owner often reads on a phone.** Give short, plain progress updates, and end any message
  that needs a decision with a numbered list of choices and a recommendation.
- **Keep the docs current:** update `docs/recompiler-design.md` (D9, D19, D20) and
  `docs/handoff.md` as you finish things.
