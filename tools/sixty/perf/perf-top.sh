#!/usr/bin/env bash
# perf-top.sh [ROUTE:FRAMES...]: real-time headless runs at 30 and then 60 fps (session top, WW-4 perf). For each:
# perf/RATE-ROUTE.frames (WWHD_FRAME_LOG), .threads (each thread's CPU % over the run's middle half),
# .rt (the "wwhd real time" lines). Timings only: no game data. One run at a time; waits while a game
# runs from ~/wwhd-test or ~/wwhd-play (the owner's).
cd ~/wwhd-test
mkdir -p perf
specs=("$@"); [ ${#specs[@]} -eq 0 ] && specs=(continue:1800 tour:2190 sail:2940 menus:1920 warp:3780)
snap() { for t in /proc/$1/task/*; do printf '%s\t%s\n' "$(cat $t/comm)" "$(sed 's/.*) //' $t/stat | awk '{print $12+$13}')"; done 2>/dev/null; }
for spec in "${specs[@]}"; do
    route=${spec%%:*}; frames=${spec#*:}
    for rate in 30 60; do
        while pgrep -f 'wwhd-(test|play)/game/wwhd.wua' >/dev/null; do sleep 5; done   # ours or the owner's game
        if [ $rate = 60 ]; then exit=$((frames * 2)); sixty=1; else exit=$frames; sixty=; fi
        log=perf/$rate-$route
        rm -f $log.frames
        WWHD_60FPS=$sixty WWHD_WINDOW=0 WWHD_SAVE=saves/wwhd_100 WWHD_FRAME_LOG=$PWD/$log.frames \
            CEMU_INPUT_SCRIPT=$PWD/routes/$route-100.txt WWHD_EXIT_FRAME=$exit ./play.sh > /tmp/wwhd-perf-top.out 2>&1 &
        pid=; while [ -z "$pid" ]; do sleep 0.2; pid=$(pgrep -n -f wwhd-test/game/wwhd.wua); done
        secs=$((frames / 30))
        sleep $((secs / 4 + 10)); snap $pid > /tmp/wwhd-perf-a; t0=$(date +%s.%N)
        sleep $((secs / 2)); snap $pid > /tmp/wwhd-perf-b; t1=$(date +%s.%N)
        while kill -0 $pid 2>/dev/null; do sleep 1; done
        python3 - /tmp/wwhd-perf-a /tmp/wwhd-perf-b $t0 $t1 > $log.threads <<'PY'
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
        grep -a "wwhd real time" portable/log.txt | cut -c32-260 > $log.rt
        echo "$(date +%H:%M) $rate $route: $(cat $log.threads)"
    done
done
echo "perf runs done"
