#!/usr/bin/env bash
# worker-ab.sh ROUTE:FRAMES ROUNDS VARIANT... - perf-ab.sh for a worker (its container, the Intel GPU
# through anv, headless; session cloud). Real-time runs of the game from the 100% save, the variants alternating,
# ROUNDS times each, one run at a time. Timings only: the frame logs and thread CPU stay in the output directory.
#   VARIANT  NAME[:K=V,K=V...]: the environment that variant adds. RATE=30|60 (default 60) is the frame rate;
#            BIN=path a binary of its own (default build/wwhd/wwhd-null: an A/B of a switch runs one binary).
#            e.g.  worker-ab.sh continue:1800 6 off:WWHD_LAZY_DRAWDONE=0 on:WWHD_LAZY_DRAWDONE=1
#                  worker-ab.sh continue:1800 4 30:RATE=30 60:RATE=60        (the baseline)
#   PERF_TAG=NAME  the output directory, $OUT/perf/NAME (default the route and the time)
#   PERF_WARMUP=0  no warm-up round (round 0: each variant's shader cache, left out of the summary)
#   PERF_NOWAIT=1  don't wait for other sessions' games on the worker to end before each run (they share its
#                  CPU and its power cap: a run beside one is noise)
#   PERF_KEEPLOG=1 keep every run's emulator log (ab-...-N.log), not only a crashed run's
#   PERF_CORE=1    let a crash write a core dump (off: a core holds game memory; delete it when done)
#   PERF_CLOCK=1   sample the GPU's clock every 0.5 s during each run (gpu-clock.sh's sysfs file: the worker's iGPU
#                  shares the package's power cap with the CPU) into ab-...-N.clock, the gameplay window in .window;
#                  worker_absum.py gives each run's mean. With WWHD_GPU_TIMING=1 in a variant, PERF_KEEPLOG=1 keeps the
#                  logs whose gameplay lines worker_absum.py sums (GPU ms a frame by kind)
# Out: ab-NAME-ROUTE-N.frames (WWHD_FRAME_LOG), .threads (each thread's CPU % over gameplay: game frame 900 to
# 95% of the run), .rt (the real-time lines). Then worker_absum.py's summary of the directory.
# A variant with WWHD_CORES=1 skips the title (WWHD_DEBUG_BOOT=1: the title's readiness in real time; not yet with three
# host threads, where it crashes at the play scene's create; a variant's WWHD_DEBUG_BOOT=0 keeps the title).
# Run it as a job: tools/worker/job start ab tools/sixty/perf/worker-ab.sh continue:1800 6 off:... on:...
set -uo pipefail
source "$(dirname "$0")/../tests/common.sh"
spec=${1:?usage: worker-ab.sh ROUTE:FRAMES ROUNDS VARIANT...}; rounds=${2:?rounds}; shift 2
[ $# -gt 0 ] || { echo "worker-ab.sh: no variants" >&2; exit 2; }
route=${spec%%:*}; frames=${spec#*:}
script=$ROOT/tools/reference/routes/$route-100.txt
[ -f "$script" ] || { echo "worker-ab.sh: no route $script" >&2; exit 2; }
dir=$OUT/perf/${PERF_TAG:-$route-$(date +%m%d-%H%M)}
mkdir -p "$dir"
echo "worker-ab.sh: $route to game frame $frames, $rounds rounds of: $* -> $dir"
echo "$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo ?) $*" > "$dir/variants"
# each variant's binary in a folder of its own (its portable dir: NAND, save, log)
declare -A bin
for v in "$@"; do
    name=${v%%:*}; b=$ROOT/build/wwhd/wwhd-null
    IFS=, read -ra kvs <<< "$([[ $v == *:* ]] && echo "${v#*:}")"
    for kv in "${kvs[@]}"; do [ "${kv%%=*}" = BIN ] && b=${kv#*=}; done
    mkdir -p "$dir/bin-$name" && cp "$b" "$dir/bin-$name/wwhd-null.new" && mv "$dir/bin-$name/wwhd-null.new" "$dir/bin-$name/wwhd-null" || exit 2
    bin[$name]=$dir/bin-$name/wwhd-null
done
[ "${PERF_CORE:-}" = 1 ] || ulimit -c 0
clockf=
if [ "${PERF_CLOCK:-}" = 1 ]; then
    for f in ${GPU_CLOCK_FILE:-/sys/class/drm/card*/gt_act_freq_mhz}; do [ -r "$f" ] && { clockf=$f; break; }; done
    [ -n "$clockf" ] || echo "worker-ab.sh: PERF_CLOCK: no Intel GPU clock in sysfs (GPU_CLOCK_FILE=...)"
fi
pid=; trap '[ -n "$pid" ] && kill "$pid" 2>/dev/null' EXIT    # job stop: the run's game too
snap() { local k; for k in /proc/$1/task/*; do printf '%s\t%s\n' "$(cat $k/comm)" "$(sed 's/.*) //' $k/stat | awk '{print $12+$13}')"; done 2>/dev/null; }
swaps() { local n; n=$(tail -c 200 "$1" 2>/dev/null | tail -1 | cut -d' ' -f1); echo "${n:-0}"; }
others() { pgrep -f 'wwhd-null -g' | grep -vx "$1" | wc -l; }
# round 0 warms each variant's shader cache (portable/shaderCache: a first run compiles in gameplay too); the
# summary leaves it out. PERF_WARMUP=0: none (the bin folders already ran)
for n in $(seq $([ "${PERF_WARMUP:-1}" = 0 ] && echo 1 || echo 0) "$rounds"); do
    for v in "$@"; do
        name=${v%%:*}; rate=60; envs=()
        IFS=, read -ra kvs <<< "$([[ $v == *:* ]] && echo "${v#*:}")"
        for kv in "${kvs[@]}"; do
            case ${kv%%=*} in RATE) rate=${kv#*=} ;; BIN) ;; *) envs+=("$kv") ;; esac
        done
        waited=0
        while [ -z "${PERF_NOWAIT:-}" ] && [ "$(others x)" -gt 0 ]; do
            [ $((waited % 12)) = 0 ] && echo "worker-ab.sh: waiting for $(others x) other game(s) on the worker ($((waited / 12)) min)"
            waited=$((waited + 1)); sleep 5
        done
        log=$dir/ab-$name-$route-$n; tb=
        rm -f "$log.frames" "$log.threads" "$log.clock" "$log.window" "$dir/.a" "$dir/.b"
        # the route's own setup: its "#env NAME=VALUE" lines (stage warps, spawns), as tools/sixty/run.sh applies them
        while IFS= read -r line; do envs+=("${line#\#env }"); done < <(grep '^#env [A-Z_0-9]*=' "$script" || true)
        if [ "$rate" = 60 ]; then sixty=1 exit=$((frames * 2)) start=1800; else sixty= exit=$frames start=900; fi
        pidf=$dir/pid-$name
        boot=(); [[ " ${envs[*]} " == *" WWHD_CORES=1 "* ]] && boot=(WWHD_DEBUG_BOOT=1)
        env WWHD_NATIVE=on WWHD_RENDER=vk "${boot[@]}" "${envs[@]}" WWHD_60FPS=$sixty WWHD_AUDIO_HASH=/dev/null WWHD_FRAME_LOG=$log.frames WWHD_EXIT_FRAME=$exit \
            CEMU_BIN="${bin[$name]}" REF_FRESH=1 REF_SAVE=/wwhd/data/saves/wwhd_100 REF_PIDFILE=$pidf \
            CEMU_INPUT_SCRIPT=$script tools/reference/run.sh > "$log.out" 2>&1 || { echo "worker-ab.sh: $name $n: no start"; continue; }
        pid=$(cat "$pidf"); t0=$(date +%s.%N); a=; t=0; clk=
        if [ "${PERF_CLOCK:-}" = 1 ] && [ -n "$clockf" ]; then
            while kill -0 "$pid" 2>/dev/null; do echo "$(date +%s.%N) $(cat "$clockf")"; sleep 0.5; done > "$log.clock" &
            clk=$!
        fi
        port=$(dirname "${bin[$name]}")/portable/log.txt
        # the frame log is written at exit: gameplay (game frame 900 on) is found from the real-time lines (one each 10 s:
        # the fps over the period, so their sum is the swaps so far); the window runs from the first line past it to the
        # last line (not the exit's teardown)
        lines=0; limit=$((exit / rate * 6 + 180))   # a hung run: six times its length at full speed
        while kill -0 "$pid" 2>/dev/null; do
            sleep 1; t=$((t + 1))
            [ $t -gt $limit ] && { echo "worker-ab.sh: $name $n still running after ${t}s: stopped"; kill "$pid"; sleep 5; kill -9 "$pid" 2>/dev/null; }
            read -r l sw < <(grep -a 'wwhd real time' "$port" 2>/dev/null |
                awk '{for (i = 2; i < NF; i++) if ($i == "fps") s += $(i-1) * $(i+2)} END {printf "%d %d", NR, s}')
            if [ "$l" -gt "$lines" ]; then
                lines=$l
                if [ -z "$a" ]; then
                    [ "$sw" -ge $start ] && { snap "$pid" > "$dir/.a"; ta=$(date +%s.%N); a=1; }
                else
                    snap "$pid" > "$dir/.b.new" && [ -s "$dir/.b.new" ] && { mv "$dir/.b.new" "$dir/.b"; tb=$(date +%s.%N); }
                fi
            fi
            [ $((t % 60)) = 0 ] && echo "worker-ab.sh: $name $n running ${t}s"
        done
        [ -n "$clk" ] && wait "$clk" 2>/dev/null
        [ -n "$a" ] && [ -n "${tb:-}" ] && echo "$ta $tb" > "$log.window"
        # a run that ended without its frame log (a crash): keep its emulator log, the next run overwrites it
        [ -s "$log.frames" ] || cp "$port" "$log.crash.txt" 2>/dev/null
        [ "${PERF_KEEPLOG:-}" = 1 ] && cp "$port" "$log.log" 2>/dev/null
        others=$(others x); [ "$others" = 0 ] && others=
        [ -n "$a" ] && [ -f "$dir/.b" ] && python3 - "$dir/.a" "$dir/.b" "$ta" "$tb" > "$log.threads" <<'PY'
import sys, collections, os
hz = os.sysconf("SC_CLK_TCK")
def load(p):
    d = collections.Counter()
    for l in open(p):
        n, c = l.rstrip("\n").split("\t"); d[n] += int(c)
    return d
a, b = load(sys.argv[1]), load(sys.argv[2]); s = float(sys.argv[4]) - float(sys.argv[3])
rows = sorted(((b[k] - a.get(k, 0)) / hz / s * 100, k) for k in b)
print(" ".join(f"{k}={c:.1f}%" for c, k in reversed(rows) if c > 0.5), f"total={sum(r[0] for r in rows):.1f}%")
PY
        grep -a "wwhd real time" "$port" | cut -c32-260 > "$log.rt" 2>/dev/null
        echo "$(date +%H:%M) $name $route $n: $(awk -v s=$(date +%s.%N) -v t=$t0 'BEGIN{printf "%.0f s", s-t}'), $(wc -l < "$log.frames" 2>/dev/null || echo 0) frames logged, load $(cut -d' ' -f1 /proc/loadavg)${others:+, $others other game(s)}"
    done
done
rm -f "$dir/.a" "$dir/.b" "$dir/.b.new"
python3 "$ROOT/tools/sixty/perf/worker_absum.py" "$dir" "$route"
echo "ab runs done: $dir"
