#!/usr/bin/env bash
# sync.sh [up|down PATH...]
#   up (default): copy this checkout's working tree (committed or not) to /wwhd/WWHDRecomp on the
#                 worker. Local-only data (orig/, ghidra/projects/, build dirs) is never sent.
#   down PATH...: copy generated results (e.g. config/US_v0/functions.csv) back from the worker.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
case "${1:-up}" in
    up)   rsync -a --delete --exclude=.git --exclude=/orig/ --exclude=/ghidra/projects/ \
                --exclude=/build/ --exclude=__pycache__ "$root/" worker:/wwhd/WWHDRecomp/
          ssh worker 'mkdir -p /wwhd/WWHDRecomp/orig && ln -sfn /wwhd/data/orig/0005000010143500_v0 /wwhd/WWHDRecomp/orig/0005000010143500_v0' ;;
    down) shift; for p in "$@"; do rsync -a "worker:/wwhd/WWHDRecomp/$p" "$root/$p"; done ;;
    *)    echo "usage: sync.sh [up|down PATH...]" >&2; exit 2 ;;
esac
