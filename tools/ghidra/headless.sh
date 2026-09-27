#!/usr/bin/env bash
# analyzeHeadless with a usable heap. Ghidra's own launcher hard-codes 2G.
#   GHIDRA_INSTALL_DIR  default ~/opt/ghidra_12.0.4_PUBLIC (12.0.x: the RPX loader targets 12.0)
#   GHIDRA_MAXMEM       default 6G
set -euo pipefail
ghidra=${GHIDRA_INSTALL_DIR:-$HOME/opt/ghidra_12.0.4_PUBLIC}
[[ -x $ghidra/support/launch.sh ]] || { echo "Ghidra not found at $ghidra (set GHIDRA_INSTALL_DIR)" >&2; exit 2; }
[[ -d $ghidra/Ghidra/Extensions/GhidraRPXLoader ]] || { echo "GhidraRPXLoader not installed in $ghidra/Ghidra/Extensions" >&2; exit 2; }
exec "$ghidra/support/launch.sh" fg jdk Ghidra-Headless "${GHIDRA_MAXMEM:-6G}" \
    "-XX:ParallelGCThreads=2 -XX:CICompilerCount=2 -Djava.awt.headless=true" \
    ghidra.app.util.headless.AnalyzeHeadless "$@"
