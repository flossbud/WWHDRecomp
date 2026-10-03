#!/usr/bin/env bash
# room_list.sh "STAGE,point,room,layer" [ROUTE]: warp there at f920 (from the save route's Outset dock
# by default) and list the processes present at f1095 (number, name, count) and Link's position: the
# first step for an area. Stage names: Outset sea room 44, Windfall sea 11, Dragon Roost Cavern
# M_NewD2, Gohma M_DragB, Forbidden Woods kindan (more in docs/handoff.md). WWHD_DEBUG_BOSS=1 for
# boss rooms on a finished save.
set -e
source "$(dirname "$0")/common.sh"
d=$OUT/room; rm -rf $d
WWHD_DEBUG_STAGE="920:$1" WWHD_STATE_TRACK=all SIXTY_FRAMES=1100 tools/sixty/run.sh ${2:-save} $d 30 > /dev/null 2>&1
python3 - $d/30/track.bin <<'PY'
import collections, struct, sys
sys.path.insert(0, "tools/sixty"); import compare
t = compare.load_track(sys.argv[1])
names = {}
for l in open("/wwhd/data/ghidra-out/actor_names.tsv"):
    if not l.startswith("#"):
        p = l.rstrip("\n").split("\t"); names[p[0]] = p[1]
T = 2190
c = collections.Counter(k[0] for k, v in t.items() if T in v)
print(", ".join(f"{p} {names.get(str(p), '?')} x{n}" for p, n in sorted(c.items())))
for k, v in t.items():
    if k[0] == 168 and T in v:
        print("Link at %.0f %.0f %.0f" % struct.unpack(">3f", v[T][0x314:0x320]))
PY
