#!/usr/bin/env bash
# movers.sh LIST: which actor types change state at 30 where they stand (the census item, session main). LIST has
# lines "TYPE STAGE ROOM" (a room a stage file places the type in: tools/stage_actors.py). Each type: a warp at f920
# to the room's first spawn point (point 0 if it has none; the default layer), the game at 30 to f1250 with
# WWHD_STATE_TRACK=TYPE, then over f1050-1250: its instances, the share of ticks on which any of their bytes
# changed, and the offsets that changed most. "not created": the room made none (another layer, a save state).
# MOVERS_JOBS runs at once (default 4), each with a binary of its own. Output: game state, it stays on the worker.
set -e
source "$(dirname "$0")/common.sh"
list=$1
M=$OUT/movers; mkdir -p "$M"
jobs=${MOVERS_JOBS:-4}
run_one() {
    local t=$1 st=$2 room=$3 slot=$4 pt
    pt=$(python3 tools/stage_actors.py "$st" --spawns 2>/dev/null | grep " in room $room at" | head -1 | sed -E 's/.*spawn point ([0-9]+) .*/\1/')
    mkdir -p "$M/bin$slot"; cp build/wwhd/wwhd-null "$M/bin$slot/"
    rm -rf "$M/$t"
    WWHD_DEBUG_STAGE="920:$st,${pt:-0},$room,-1" WWHD_STATE_TRACK=$t SIXTY_FRAMES=1250 CEMU_BIN="$M/bin$slot/wwhd-null" \
        SIXTY_OUT="$M/slot$slot" tools/sixty/run.sh save "$M/$t" 30 > "$M/$t.log" 2>&1 || true
    echo "$st,${pt:-0},$room" > "$M/$t.where"
}
slot=0
while read -r t st room; do
    [ -z "$t" ] && continue
    run_one "$t" "$st" "$room" $((slot % jobs)) &
    slot=$((slot + 1))
    [ $((slot % jobs)) -eq 0 ] && wait
done < "$list"
wait
python3 - "$M" "$list" <<'PY'
import collections, os, sys
sys.path.insert(0, "tools/sixty")
import compare
M, lst = sys.argv[1], sys.argv[2]
for line in open(lst):
    if not line.split():
        continue
    t = int(line.split()[0])
    where = open(os.path.join(M, f"{t}.where")).read().strip() if os.path.exists(os.path.join(M, f"{t}.where")) else "?"
    try:
        T = compare.load_track(os.path.join(M, str(t), "30", "track.bin"))
    except Exception as e:
        print(f"{t:4d} {where:22s} no track ({e.__class__.__name__})"); continue
    keys = [k for k in T if k[0] == t and any(2100 <= f <= 2500 for f in T[k])]
    if not keys:
        print(f"{t:4d} {where:22s} not created"); continue
    ticks = changed = 0
    offs = collections.Counter()
    for k in keys:
        fs = [f for f in sorted(T[k]) if 2100 <= f <= 2500 and f % 2 == 0]
        for a, b in zip(fs, fs[1:]):
            x, y = T[k][a], T[k][b]
            ticks += 1
            d = [o for o in range(0, min(len(x), len(y)), 4) if x[o:o + 4] != y[o:o + 4]]
            changed += bool(d)
            offs.update(d)
    top = ", ".join(f"+{o:#x}" for o, _ in offs.most_common(6))
    print(f"{t:4d} {where:22s} x{len(keys):<3d} changes on {100 * changed / max(ticks, 1):5.1f}% of ticks  {top}")
PY
