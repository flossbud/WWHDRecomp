#!/usr/bin/env bash
# probe_menu.sh NAME [ENV=VALUE...] - one 60-tick tour run with the given environment, compared with
# the 30-tick baseline in /wwhd/data/m6/ui/30: the first tick any actor differs, and when the pause
# menu's open/close voices start (game frames). A bisection probe for D21's menu-close tick.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
name=${1:?usage: probe_menu.sh NAME [ENV=VALUE...]}; shift
out=/wwhd/data/m6/probe-$name
env "$@" SIXTY_TRACE=snd_core.AXSetVoiceState "$here/run.sh" tour "$out" 60 | grep -v "actor states" | tail -1
voices=$(uv run "$here/../reference/hle_trace.py" dump "$out/60/trace.zst" --grep AXSetVoiceState 2>/dev/null |
    awk '{print $2}' | tail -2 | awk '{printf "%.1f ", 900 + ($1 - 900) / 2}')
first=$(python3 "$here/compare.py" /wwhd/data/m6/ui/30 "$out/60" --from 880 --fields 0 2>/dev/null |
    awk '/per process name/ {p=1; next} p && /first/ {print $NF; exit}')
echo "probe $name: menu voices at game frames ${voices}(30: 1564 1983); first actor difference: ${first:-none}"
