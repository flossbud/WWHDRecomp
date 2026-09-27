#!/usr/bin/env bash
# Rebuild the disposable Ghidra project from scratch and refresh config/US_v0/functions.csv:
# import + auto-analysis, seed functions from relocations, apply symbols.csv, export.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
"$here/import.sh" "$@"
uv run -q --script "$here/seed_functions.py"
uv run -q --script "$here/apply_symbols.py"
uv run -q --script "$here/export_functions.py"
