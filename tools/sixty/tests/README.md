# 60 fps tests (WW-4, D21)

Run on the worker from a worker checkout (`tools/worker/job start NAME tools/sixty/tests/X.sh ...`).
Outputs go to `/wwhd/data/m6/<checkout name>/` (game memory and captures: they stay on the worker).

- `spawn_test.sh PROC`: spawn an actor on the Outset dock, trial, 30 against 60.
- `stage_test.sh PROC STAGE,point,room,layer FROM TO [SPAWN]`: the same after a stage warp
  (`WWHD_DEBUG_BOSS=1` for bosses on a finished save).
- `route_test.sh ROUTE PROC FROM TO`: on a route (NPCs, objects).
- `room_list.sh STAGE,point,room,layer`: warp and list the processes there (start of an area).
- `paths.py DIR FROM TO [OFFSET]`: 30 against 60 by half frame (positions, or e.g. 0x390, the eye).
- `regress.sh`: every converted enemy again, after shared changes.
- `checks.sh NAME`: the switch-off checks every commit needs (all MATCH, diff 0 mismatches,
  captures identical).
- `shots.sh ROUTE FRAMES`, `framediff.sh ROUTE FIRST LAST`: captures; frame-by-frame change.
- `src_of.py DIR ADDR...`: where a store's value was computed (where a step rule goes).

The workflow per actor type is in docs/handoff.md ("Converting an actor").
