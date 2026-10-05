#!/usr/bin/env bash
# sync.sh [up|down PATH...]   (WWHD_ON=desktop: to/from the desktop's worker, tools/worker/target.sh)
#   up (default): copy this checkout's working tree (committed or not) to its worker checkout:
#                 /wwhd/WWHDRecomp, or the path in this checkout's .worker-dir (gitignored; one per
#                 parallel session, each with its own build/). Local-only data (orig/,
#                 ghidra/projects/, build dirs) is never sent; a new worker checkout gets a copy of
#                 /wwhd/WWHDRecomp's Ghidra project.
#   down PATH...: copy generated results (e.g. config/US_v0/functions.csv) back from the worker.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
dir=$(cat "$root/.worker-dir" 2>/dev/null || echo /wwhd/WWHDRecomp)
source "$root/tools/worker/target.sh"        # WWHD_ON=desktop: the desktop's worker
hdir=$(hostpath "$dir")
case "${1:-up}" in
    up)   on_host "mkdir -p $hdir"                # made by the host's user (a podman-made one isn't writable here)
          rsync -a --delete --exclude=.git --exclude=/orig/ --exclude=/ghidra/projects/ \
                --exclude=/build/ --exclude=__pycache__ --exclude=/.worker-dir --exclude=/.session \
                "$root/" "$W_SSH:$hdir/"
          on_host "mkdir -p $hdir/orig && ln -sfn /wwhd/data/orig/0005000010143500_v0 $hdir/orig/0005000010143500_v0 &&
              if [ ! -d $hdir/ghidra/projects ] && [ -d $W_ROOT/WWHDRecomp/ghidra/projects ] && [ $dir != /wwhd/WWHDRecomp ]; then mkdir -p $hdir/ghidra && cp -a $W_ROOT/WWHDRecomp/ghidra/projects $hdir/ghidra/; fi" ;;
    down) shift; for p in "$@"; do rsync -a "$W_SSH:$hdir/$p" "$root/$p"; done ;;
    *)    echo "usage: sync.sh [up|down PATH...]" >&2; exit 2 ;;
esac
