#!/usr/bin/env bash
# Import + auto-analyse cking.rpx into a disposable project at ghidra/projects/wwhd.
# Re-running replaces the program; names/types are re-applied from config/ by script.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
rpx=${1:-$root/orig/0005000010143500_v0/code/cking.rpx}
[[ -f $rpx ]] || { echo "missing $rpx — see orig/README.md" >&2; exit 2; }
mkdir -p "$root/ghidra/projects"
exec "$root/tools/ghidra/headless.sh" "$root/ghidra/projects" wwhd \
    -import "$rpx" -overwrite \
    -log "$root/ghidra/projects/import.log" -scriptlog "$root/ghidra/projects/script.log"
