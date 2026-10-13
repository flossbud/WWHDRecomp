#!/usr/bin/env bash
# mac.sh start|stop|status|image: the arm64 worker, an arm64 Debian VM on the owner's laptop (m-android: the arm64
# build and its checks run natively there, not under qemu). The same wwhd-worker image (tools/worker/Dockerfile,
# built there for arm64: mac.sh image) under rootless podman, /wwhd being the VM's /wwhd. Only for jobs that ask for
# it (WWHD_ON=mac); the owner takes the laptop back at any time: then stop, and arm64 jobs go back to the worker's
# cross build (tools/arm64/build.sh) and qemu. The VM has ~12 GB: JOBS=6 for big builds.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
source "$here/hosts.sh"
host=${WWHD_MAC_SSH:?WWHD_MAC_SSH in tools/worker/hosts.env}
on() { ssh -o BatchMode=yes -o ConnectTimeout=10 "$host" "$@"; }
case "${1:-status}" in
image)
    on 'mkdir -p /wwhd/worker'
    rsync -a "$here/Dockerfile" "$host:/wwhd/worker/Dockerfile"
    on 'podman build -t wwhd-worker:latest /wwhd/worker' ;;
start)
    on 'set -e
podman rm -f wwhd-worker >/dev/null 2>&1 || true
mkdir -p /wwhd/logs /wwhd/data/m6 /wwhd/home /wwhd/cache /wwhd/WWHDRecomp   # made by this user: a podman-made one is not writable
podman run -d --name wwhd-worker --init --userns=keep-id:uid=1000,gid=1000 \
    --memory 10g --memory-swap 10g --pids-limit 8192 --shm-size 2g \
    -v /wwhd:/wwhd --hostname wwhd-mac wwhd-worker:latest >/dev/null
podman exec wwhd-worker id
echo "mac worker on"' ;;
stop)
    on 'podman stop -t 10 wwhd-worker >/dev/null 2>&1 || true; echo "mac worker off"' ;;
status)
    on 'if [ "$(podman container inspect -f "{{.State.Running}}" wwhd-worker 2>/dev/null)" = true ]; then
    echo "mac worker on"; else echo "mac worker off"; fi' ;;
*) echo "usage: mac.sh start|stop|status|image" >&2; exit 2 ;;
esac
