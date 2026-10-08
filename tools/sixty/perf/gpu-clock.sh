#!/usr/bin/env bash
# gpu-clock.sh OUT [SECONDS] - the GPU's clock during a game run (docs/research/gpu-plan.md item 1): on the worker
# the CPU and the iGPU share the package's power cap, so a busier CPU can slow the GPU. Samples every 0.5 s while a
# game (wwhd-null -g) runs: it waits up to SECONDS (default 600) for one to start, then samples until none is left.
# Writes "SECONDS_SINCE_START MHZ" lines to OUT and prints the mean, the 10th percentile and the minimum.
# Intel: /sys/class/drm/card*/gt_act_freq_mhz (the actual frequency); AMD: the starred line of
# /sys/class/drm/card*/device/pp_dpm_sclk. Intel's comes first: on a machine with both (the desktop worker's CPU has
# an iGPU too) name the one the game renders on: GPU_CLOCK_FILE=/sys/class/drm/card2/device/pp_dpm_sclk.
# Run it as a job beside a run (it only reads sysfs):
#   tools/worker/job start clock tools/sixty/perf/gpu-clock.sh /wwhd/data/m6/.../clock.txt
set -uo pipefail
out=${1:?usage: gpu-clock.sh OUT [SECONDS]}; wait_s=${2:-600}
src=${GPU_CLOCK_FILE:-}; kind=
case "$src" in *gt_act_freq_mhz) kind=intel ;; *pp_dpm_sclk) kind=amd ;; esac
[ -z "$src" ] && for f in /sys/class/drm/card*/gt_act_freq_mhz; do [ -r "$f" ] && { src=$f; kind=intel; break; }; done
if [ -z "$src" ]; then
    for f in /sys/class/drm/card*/device/pp_dpm_sclk; do [ -r "$f" ] && { src=$f; kind=amd; break; }; done
fi
[ -n "$src" ] || { echo "gpu-clock.sh: no GPU clock in sysfs here" >&2; exit 2; }
read_mhz() {
    if [ "$kind" = intel ]; then cat "$src"
    else sed -n 's/.*: *\([0-9]*\)Mhz *\*.*/\1/p' "$src" | head -1; fi
}
games() { pgrep -f 'wwhd-null -g' | wc -l; }
echo "gpu-clock.sh: $src ($kind) -> $out; waiting for a game"
for ((i = 0; i < wait_s * 2; i++)); do [ "$(games)" -gt 0 ] && break; sleep 0.5; done
[ "$(games)" -gt 0 ] || { echo "gpu-clock.sh: no game started in ${wait_s}s"; exit 1; }
: > "$out"
t0=$(date +%s.%N); n=0
while [ "$(games)" -gt 0 ]; do
    printf '%s %s\n' "$(awk -v a="$(date +%s.%N)" -v b="$t0" 'BEGIN{printf "%.1f", a-b}')" "$(read_mhz)" >> "$out"
    n=$((n + 1)); [ $((n % 120)) = 0 ] && echo "gpu-clock.sh: $n samples"
    sleep 0.5
done
sort -n -k2 "$out" | awk '{v[NR]=$2; s+=$2} END {if (NR) printf "gpu-clock.sh: %d samples: mean %.0f MHz, 10th percentile %d, min %d\n", NR, s/NR, v[int(NR*0.1)+1], v[1]}'
