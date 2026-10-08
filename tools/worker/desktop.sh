#!/usr/bin/env bash
# desktop.sh start|stop|status: the second worker on the owner's desktop (24 threads, 31 GB),
# the same wwhd-worker image under rootless podman, /wwhd being ~/wwhd-desk there
# (tools/worker/target.sh: WWHD_ON=desktop sends sync.sh, w and job to it).
#   start   (re)create the container: all 24 threads (the owner lends the whole desktop), 28 GB of
#           the 31 (an out-of-memory job dies inside the container, the desktop stays up),
#           and keep the desktop from sleeping while it runs (a user unit, wwhd-awake)
#   stop    the owner wants the desktop: stop the container (its running jobs end) and let it sleep
#           again. Jobs then go to the worker (`job start` refuses the desktop while it's off).
#   status  on or off, load, running jobs
# The desktop has the game, saves, tools and caches (~10 GB, copied from the worker's /wwhd once),
# not the reference traces and captures: tools/sixty/tests/checks.sh runs on the worker only.
set -euo pipefail
source "$(dirname "$0")/desktop-env.sh"     # the desktop's login (tools/worker/desktop.env)
host=$WWHD_DESKTOP_SSH
on() { ssh -o BatchMode=yes -o ConnectTimeout=10 "$host" "$@"; }
case "${1:-status}" in
start)
    on 'set -e
podman rm -f wwhd-worker >/dev/null 2>&1 || true
mkdir -p ~/wwhd-desk/logs ~/wwhd-desk/data/m6
podman run -d --name wwhd-worker --init --userns=keep-id --group-add keep-groups \
    --cpus 24 --memory 28g --memory-swap 28g --pids-limit 16384 --shm-size 4g \
    --device /dev/dri -v "$HOME/wwhd-desk:/wwhd:z" --hostname wwhd-desk \
    wwhd-worker:latest >/dev/null
systemctl --user stop wwhd-awake 2>/dev/null || true
systemd-run --user --unit=wwhd-awake --description="Keep the desktop awake for the WWHD worker" \
    systemd-inhibit --what=sleep:idle --who="WWHD worker" --why="WWHD sessions run jobs here" --mode=block sleep infinity >/dev/null
podman exec wwhd-worker id
echo "desktop worker on"' ;;
stop)
    on 'podman stop -t 10 wwhd-worker >/dev/null 2>&1 || true; systemctl --user stop wwhd-awake 2>/dev/null || true; echo "desktop worker off (the desktop may sleep again)"' ;;
status)
    on 'if [ "$(podman container inspect -f "{{.State.Running}}" wwhd-worker 2>/dev/null)" = true ]; then
            echo "desktop worker on; load $(cut -d" " -f1-3 /proc/loadavg); awake: $(systemctl --user is-active wwhd-awake)"
            for l in ~/wwhd-desk/logs/*.log; do [ -e "$l" ] || continue; grep -q "^EXIT=" "$l" || echo "  running: $(basename "$l" .log)"; done
        else echo "desktop worker off"; fi' ;;
*) echo "usage: desktop.sh start|stop|status" >&2; exit 2 ;;
esac
