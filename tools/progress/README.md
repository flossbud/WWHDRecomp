# Progress page

A small status page for the owner (desktop and phone), served from the worker's host on the tailnet:
**port 8765 on the worker's tailnet address** (bound to that address only; `WWHD_PROGRESS_URL` in
`tools/worker/hosts.env`).

- `index.html`: the page (no external files). It reads `progress.json`, `sessions.json` (the "working
  on" lines), `shots.json` and `shots-archive.json`, `claims.json` (the queue) and `bugs.json` next to it every 20 seconds.
- `testing.html`: the owner's testing page: fixed bugs to retest (verified / still broken), new bugs and
  notes with pasted images, finished queue items to check in play (`checks.json`), general notes (`notes.json`).
- `server.py`: the server (installed as `.server.py` beside the data by `publish.sh serve`): the files as
  static, plus the API the testing page writes through (bugs.json under `.claims.lock` like `publish.sh bug`;
  images in `notes/`). Sessions read the owner's entries with `publish.sh bug list|show` and `publish.sh notes`.
- `collect.py`: builds `progress.json` from the repo (default conversions, tick rules, overrides,
  symbols, functions, recent commits) and `plan.json` (milestones, actor groups, known issues).
- `publish.sh`: `publish.sh` publishes the data and the page; `publish.sh now TEXT` sets the
  "working on" line (cheap: call it at each step); `publish.sh retire [SESSION]` takes a session's line off
  the page when the session ends; `publish.sh step ID DONE TOTAL` sets a queue item's progress bar
  (items with "ids" in `plan.json`, the actor types they cover, fill theirs on their own);
  `publish.sh shot PPM CAPTION` adds a capture made
  on the worker; `publish.sh usage` copies the Claude account's usage meters from the editing machine into `usage.json` (a crontab entry
  there, tagged `wwhd-usage`, runs it every 5 minutes; only percentages and reset times leave that machine);
  `publish.sh serve` installs `server.py` and starts it, or restarts it when it changed (a crontab
  `@reboot` entry on the worker, tagged `wwhd-progress`, starts `.server.py` after a reboot).

The data lives in `/wwhd/data/progress` on the worker, outside git. Every capture is kept: `shots.json`
has the newest 24 and `shots-archive.json` the rest (the page shows them 12 to a page). Captures are game data: they
become JPEGs there and never touch the editing machine or the repo. Update `plan.json` when a milestone is
done or an issue is found or fixed.
