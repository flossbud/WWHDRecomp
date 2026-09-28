#!/usr/bin/env bash
# Rebuild the disposable Ghidra project from scratch and refresh config/US_v0/{jump_tables,functions}.csv:
# import + auto-analysis, GHS switch tables, seed functions from relocations, split
# non-contiguous bodies, apply symbols.csv, export. Check the result with tools/audit_functions.py.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
"$here/import.sh" "$@"
python3 "$here/../jump_tables.py" ${1:+"$1"}
uv run -q --script "$here/apply_jump_tables.py"
uv run -q --script "$here/seed_functions.py" ${1:+--rpx "$1"}
# seeding re-runs auto-analysis, which can start functions at case labels again
uv run -q --script "$here/apply_jump_tables.py"
uv run -q --script "$here/split_functions.py"
uv run -q --script "$here/apply_symbols.py"
uv run -q --script "$here/export_functions.py"
