# Progress page

A small status page for the owner (desktop and phone), served from the worker host on the tailnet:
**http://WORKER_ADDR:8765** (bound to the tailnet address only).

- `index.html`: the page (no external files). It reads `progress.json`, `sessions.json` (the "working
  on" lines), `shots.json`, `claims.json` (the queue) and `bugs.json` next to it every 20 seconds.
- `collect.py`: builds `progress.json` from the repo (default conversions, tick rules, overrides,
  symbols, functions, recent commits) and `plan.json` (milestones, actor groups, known issues).
- `publish.sh`: `publish.sh` publishes the data and the page; `publish.sh now TEXT` sets the
  "working on" line (cheap: call it at each step); `publish.sh retire [SESSION]` takes a session's line off
  the page when the session ends; `publish.sh shot PPM CAPTION` adds a capture made
  on the worker; `publish.sh serve` starts the server (a crontab `@reboot` entry on the worker, tagged
  `wwhd-progress`, starts it after a reboot).

The data lives in `/wwhd/data/progress` on the worker, outside git. Captures are game data: they
become JPEGs there and never touch the editing machine or the repo. Update `plan.json` when a milestone is
done or an issue is found or fixed.
