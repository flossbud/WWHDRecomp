#!/usr/bin/env bash
# predeploy.sh [ROUTE...]: the smoke test before a build goes to the owner's copy (session qa). Scripted routes
# played at 30 and at 60 with the current build (build/wwhd/wwhd-null), and a verdict per route from where
# Link is: FAIL when he ends more than PREDEPLOY_FAIL units (default 150) from where the 30-tick run ends,
# i.e. the route had another outcome at 60 (the Ballad of Gales that never warped, bug B28: every test until
# then compared a build with the one before, and "unchanged" was still broken); WARN from PREDEPLOY_WARN (40).
# Also printed: the farthest apart on the way and his action at the end. Exit status 1 on a FAIL. 30's tick k
# is compared with the 60 run's half tick that holds the same tick's state: key 2k+1 or 2k-1 (a track's key is the
# tick x 2, + 1 on a half tick, and which side the 60 run's ticks fall on varies by run), whichever Link's path
# matches best (session top, walkstart: before, the same key, 60's whole frame, half a tick on: a walk at 14
# units a tick read ~7 apart).
# Default routes: warp (the Ballad of Gales to the Tower of the Gods), back and door (dungeon doors), talk,
# items (a chest), cuts, leaf, hook, ladder, crawl, carry, spin, bow, shield, swing, land, sidle, pot, plants,
# slash, sail, menus, tour, house (out of a Windfall house's door and back in), and Link's items (session top's
# items2: boomerang, bombs, grapple, boots, hammer, armor, mirror; heavy, the Power Bracelets' lift, session
# main's route), and the dungeons' (session top's dungeons2: drcjar, fwbud, tgbeam, wtspring, gtrock; medli, session
# main's); back, talk and house start from their stage warps, the items' and dungeons' routes warp, spawn and poke
# what they need (their "#env" lines, tools/sixty/run.sh). On 2026-10-05 (ww-4 14a98dd) all
# passed but warp (262,106 units: B28; 17 with its fix, 29b66d9); the largest others were land 99, swing 70,
# sail 65, sidle 53, plants 46 (WARN: known small drifts), the rest under 33. On 2026-10-06 (items2, on ww-4
# b924f3f) all 32 passed but land 82, sidle 62 and sail 51 (WARN, known); the item routes' ends 0-12 units. Not in it: walk without a stage warp (Link swims off the
# dock for 1900 ticks with camera-relative strokes: 7000 units apart, the runs' headings drift) and mob (a
# fight: the random stream).
# The 30-tick runs are kept in $OUT/predeploy/ROUTE/30 and reused (30 is the same with every build: the
# checks prove it); REDO30=1 plays them again (after a route's file or the 60 switch's start changed).
# PREDEPLOY_JOBS routes run at once (default 4), each with a copy of the binary in a folder of its own.
# Output: game state, it stays on the worker.
set -e
source "$(dirname "$0")/common.sh"
routes=("$@"); [ ${#routes[@]} -eq 0 ] && routes=(warp back door house talk items cuts leaf hook ladder crawl carry spin bow shield swing land sidle pot plants slash sail menus tour boomerang bombs grapple boots hammer armor mirror heavy drcjar fwbud tgbeam wtspring medli gtrock)
P=$OUT/predeploy; mkdir -p "$P"
jobs=${PREDEPLOY_JOBS:-4}
run_one() {
    local r=$1 slot=$2 w= rates=60 n=
    # warp ends at its arrival (f2460): the cyclone sets the boat down facing 28 degrees elsewhere at 60 (its
    # spin's last turns), so the route's sail into the tower after it goes another way: a heading, not a failure
    case $r in back) w=920:M_NewD2,5,14,-1 ;; talk) w=920:sea,0,11,-1 ;; house) w=920:sea,9,11,-1 ;; warp) n=2460 ;; esac
    mkdir -p "$P/bin$slot"; cp build/wwhd/wwhd-null "$P/bin$slot/"
    { [ -f "$P/$r/30/track.bin" ] && [ -z "${REDO30:-}" ]; } || rates="30 60"
    env ${w:+WWHD_DEBUG_STAGE=$w} ${n:+SIXTY_FRAMES=$n} WWHD_STATE_TRACK=168 CEMU_BIN="$P/bin$slot/wwhd-null" SIXTY_OUT="$P/slot$slot" \
        tools/sixty/run.sh "$r" "$P/$r" $rates > "$P/$r.log" 2>&1 || echo "predeploy: $r: the run failed (see $P/$r.log)"
}
slot=0
for r in "${routes[@]}"; do
    run_one "$r" $((slot % jobs)) &
    slot=$((slot + 1))
    [ $((slot % jobs)) -eq 0 ] && wait
done
wait
python3 - "$P" "${PREDEPLOY_FAIL:-150}" "${PREDEPLOY_WARN:-40}" "${routes[@]}" <<'PY'
import math, os, struct, sys
sys.path.insert(0, "tools/sixty")
import compare
P, fail, warn = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
bad = 0
def link(path):
    T = compare.load_track(path)
    out = {}
    for k in T:
        if k[0] == 168:
            out.update(T[k])
    return out
pos = lambda b: struct.unpack(">3f", b[0x314:0x320])
for r in sys.argv[4:]:
    try:
        a, b = link(os.path.join(P, r, "30", "track.bin")), link(os.path.join(P, r, "60", "track.bin"))
    except OSError as e:
        print(f"FAIL  {r:7s} no track ({e})"); bad += 1; continue
    # 30's tick k (key 2k) against 60's frame holding the same tick's state: a half tick's key, 2k+1 or 2k-1
    # by the run (where the 60 switch fell against the game's ticks), the one Link's path matches best
    def paired(off):
        ts = [t for t in sorted(a) if t % 2 == 0 and t + off in b and t >= 1800]
        return ts, [math.dist(pos(a[t]), pos(b[t + off])) for t in ts]
    if not a or not b or max(a) // 2 != max(b) // 2:
        print(f"FAIL  {r:7s} the runs end on different ticks: 30 at {max(a) // 2 if a else '-'}, 60 at {max(b) // 2 if b else '-'}"); bad += 1; continue
    off = min((1, -1), key=lambda o: (lambda ts, d: sum(d) / len(d) if d else float("inf"))(*paired(o)))
    ts, d = paired(off)
    if not ts:
        print(f"FAIL  {r:7s} no ticks to compare"); bad += 1; continue
    act = lambda x: struct.unpack(">I", x[max(x)][0x65F0:0x65F4])[0]
    verdict = "FAIL" if d[-1] > fail else "WARN" if d[-1] > warn else "ok"
    bad += verdict == "FAIL"
    print(f"{verdict:5s} {r:7s} Link at the end {d[-1]:8.1f} units from 30's (farthest on the way {max(d):9.1f}, mean {sum(d) / len(d):7.1f});"
          f" his action at the end {act(a):02x} at 30, {act(b):02x} at 60; ticks 900-{ts[-1] // 2}, 60's key 2k{off:+d}")
sys.exit(1 if bad else 0)
PY
