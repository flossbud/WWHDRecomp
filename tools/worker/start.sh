#!/usr/bin/env bash
# (Re)create the long-lived worker container on the worker with its resource caps:
#   24 GB RAM with no swap, 10 of 12 CPU threads, 4096 processes, and the Intel iGPU
#   (/dev/dri). The host keeps ~7 GB and 2 threads for its own jobs. Disk is the 250 GB /wwhd
#   loop volume (/var/lib/wwhd/wwhd.img, see README.md), so the worker can't fill the host's root disk.
# An out-of-memory job is killed inside the container; the host and the editing machine are unaffected.
set -euo pipefail
source "$(dirname "$0")/hosts.sh"           # the worker's address (tools/worker/hosts.env)
ssh "$WWHD_WORKER_SSH" 'set -e
mkdir -p /wwhd/home /wwhd/cache /wwhd/opt /wwhd/WWHDRecomp
docker rm -f wwhd-worker >/dev/null 2>&1 || true
docker run -d --name wwhd-worker --init --restart unless-stopped \
    --memory 24g --memory-swap 24g --cpus 10 --pids-limit 4096 --shm-size 2g \
    --device /dev/dri --group-add 44 --group-add 992 \
    -v /wwhd:/wwhd \
    --hostname wwhd-worker \
    wwhd-worker:latest
docker inspect wwhd-worker --format "{{.Name}} mem={{.HostConfig.Memory}} swap={{.HostConfig.MemorySwap}} cpus={{.HostConfig.NanoCpus}} status={{.State.Status}}"'
