#!/usr/bin/env bash
# sync.sh [up|down PATH...]
#   up (default): copy this checkout's working tree (committed or not) to its worker checkout:
#                 /wwhd/WWHDRecomp, or the path in this checkout's .worker-dir (gitignored; one per
#                 parallel session, each with its own build/). Local-only data (orig/,
#                 ghidra/projects/, build dirs) is never sent; a new worker checkout gets a copy of
#                 /wwhd/WWHDRecomp's Ghidra project.
#   down PATH...: copy generated results (e.g. config/US_v0/functions.csv) back from the worker.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
dir=$(cat "$root/.worker-dir" 2>/dev/null || echo /wwhd/WWHDRecomp)
case "${1:-up}" in
    up)   rsync -a --delete --exclude=.git --exclude=/orig/ --exclude=/ghidra/projects/ \
                --exclude=/build/ --exclude=__pycache__ --exclude=/.worker-dir --exclude=/.session \
                "$root/" "worker:$dir/"
          ssh worker "mkdir -p $dir/orig && ln -sfn /wwhd/data/orig/0005000010143500_v0 $dir/orig/0005000010143500_v0 &&
              if [ ! -d $dir/ghidra/projects ] && [ $dir != /wwhd/WWHDRecomp ]; then mkdir -p $dir/ghidra && cp -a /wwhd/WWHDRecomp/ghidra/projects $dir/ghidra/; fi" ;;
    down) shift; for p in "$@"; do rsync -a "worker:$dir/$p" "$root/$p"; done ;;
    *)    echo "usage: sync.sh [up|down PATH...]" >&2; exit 2 ;;
esac
