# tools/sixty/perf - real-time timing (WW-4's performance work)

Copies of the scripts sessions top and qa used on the owner's desktop (`~/wwhd-test`, a deployed build: `tools/play/deploy.sh`
to that directory), committed so other machines can use them. They time real-time, headless runs; they print timings
only, no game data.

| File | What |
|---|---|
| `perf-top.sh [ROUTE:FRAMES...]` | each route at 30 and then 60: `perf/RATE-ROUTE.frames` (`WWHD_FRAME_LOG`), each thread's CPU %, the real-time lines |
| `perf-ab.sh ROUTE:FRAMES [ROUNDS] [VARIANTS]` | binaries `./wwhd-null.a`, `./wwhd-null.b` (or VARIANTS) at 60, alternating, ROUNDS times |
| `worker-ab.sh ROUTE:FRAMES ROUNDS VARIANT...` | the worker's version (the worker: in its container, the Intel GPU, headless; session cloud): variants `NAME:K=V,...` (`RATE=30`, `BIN=path`, any switch), alternating, a warm-up round first, one run at a time (it waits for other sessions' games); outputs in `/wwhd/data/m6/<checkout>/perf/TAG/` |
| `worker_absum.py DIR [ROUTE]` | its summary: per run and per variant, and each variant against the first, round by round (paired) |
| `perfsum.py`, `absum.py`, `frames.py` | summaries of the `.frames` logs, gameplay only (`docs/research/perf-baseline.md` says how they were read) |

As written they run on the desktop's host (`cd ~/wwhd-test`, the GPU through RADV). For the worker, run them in its worker
container against a deployed-style directory there (the Intel GPU through anv, headless), and compare variants of the same
build only. the worker is power-capped and noisy: use more rounds. A cloud session's first step is to make a the worker variant
(docs/cloud-handoff.md, tailnet mode): `worker-ab.sh`, e.g. `tools/worker/job start ab tools/sixty/perf/worker-ab.sh
continue:1800 6 off:WWHD_LAZY_DRAWDONE=1,WWHD_SETCACHE=0 on:WWHD_LAZY_DRAWDONE=1` (`docs/research/perf-baseline.md`,
"the worker" has its baseline and how to read it).
