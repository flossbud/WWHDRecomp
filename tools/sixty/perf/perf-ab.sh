#!/usr/bin/env bash
# perf-ab.sh ROUTE:FRAMES [ROUNDS] [VARIANTS]: real-time headless runs at 60 of ./wwhd-null.V for each variant V
# (default "a b"), alternating, ROUNDS times (session top, WW-4 perf): perf/ab-V-ROUTE-N.frames. The environment
# is play.sh's (WWHD_WINDOW=0). Waits while a game runs from ~/wwhd-test or ~/wwhd-play (the owner's).
cd ~/wwhd-test
spec=$1; rounds=${2:-2}; variants=${3:-a b}
route=${spec%%:*}; frames=${spec#*:}
for n in $(seq 1 $rounds); do
    for v in $variants; do
        while pgrep -f 'wwhd-(test|play)/game/wwhd.wua' >/dev/null; do sleep 5; done
        log=perf/ab-$v-$route-$n
        rm -f $log.frames
        save=$PWD/portable/mlc01/usr/save/00050000/10143500/user/80000001
        rm -rf "$save" && mkdir -p "$save" && cp saves/wwhd_100/*.sav "$save/"
        env WWHD_NATIVE=on WWHD_RENDER=vk WWHD_CEMU_DATA=$PWD/cemu CEMU_NO_GAMEPAD=1 WWHD_AUDIO_HASH=/dev/null WWHD_60FPS=1 WWHD_60FPS_UIANIM=${UIANIM:-1} \
            WWHD_FRAME_LOG=$PWD/$log.frames CEMU_INPUT_SCRIPT=$PWD/routes/$route-100.txt WWHD_EXIT_FRAME=$((frames * 2)) \
            ./wwhd-null.$v -g $PWD/game/wwhd.wua > /tmp/wwhd-ab-top.out 2>&1
        echo "$(date +%H:%M) $v $route $n: exit $?"
    done
done
echo "ab runs done"
