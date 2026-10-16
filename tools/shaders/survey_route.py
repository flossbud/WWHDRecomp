"""A stage survey for the shader list (s-shadercover): warp into every room that has a player spawn point and turn
the camera round, so a capturing run (WWHD_SHADER_SOURCES) meets each room's shaders.

Usage (on the worker; reads tools/stage_actors.py --spawns output, which comes from the game's files):
    python3 tools/shaders/survey_route.py SPAWNS... --out DIR [--chunks N] [--visit FRAMES] [--skip STAGE[:ROOM],...]
        [--rooms FILE]

SPAWNS are files of `stage_actors.py STAGE --spawns` lines ("STAGE (ROOM) spawn point P in room R at ... (params X)").
With --rooms (lines "STAGE ROOM", the rooms that have an archive: python3 tools/shaders/survey_route.py --list-rooms on
the worker prints them, from tools/stage_actors.py's lookup, loose or packed) only those rooms are visited: a stage's file
can hold spawns for rooms the game doesn't have (Cave08's), and a warp there crashes in the room scene's create
(f_025B3F90: no file list). Each room gets one spawn: a plain one (the params' type nibble, bits 12-15, 0) if it has one, else its first. The rooms
are split into N chunks, each a run of its own: DIR/chunkK.env holds WWHD_DEBUG_STAGE (tick:STAGE,point,room,-1, a
warp every --visit frames from f920, the save route's Outset dock) and SIXTY_FRAMES, DIR/chunkK.txt the input script
(continue-100.txt's presses to the game, then the right stick held left for half of each visit, from a third of the
way in, to turn the camera round Link), DIR/chunkK.list the visits. Frames are game frames at 30 (the route runs at
30: shaders and pipelines don't depend on the rate). Stage and room numbers only: no game content.
"""
import argparse
import os
import re
import sys

LINE = re.compile(r"^(\S+) \((\d+|stage)\) spawn point (\d+) in room (\d+) at .* \(params ([0-9a-f]+)\)$")
BOOT = ["420 A 5", "540 A 5", "660 A 5", "780 A 5"]     # continue-100.txt: title, controller, file, start


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("spawns", nargs="*")
    ap.add_argument("--out")
    ap.add_argument("--rooms", help="lines STAGE ROOM: the rooms that have an archive")
    ap.add_argument("--list-rooms", action="store_true", help="print every room archive's STAGE ROOM and stop")
    ap.add_argument("--chunks", type=int, default=8)
    ap.add_argument("--visit", type=int, default=270)
    ap.add_argument("--skip", default="", help="STAGE or STAGE:ROOM, comma separated")
    a = ap.parse_args()
    if a.list_rooms:
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
        import glob
        import stage_actors
        names = {os.path.basename(p) for p in glob.glob(os.path.join(stage_actors.CONTENT, "*_Room*.szs"))}
        for pack in glob.glob(os.path.join(stage_actors.PACKS, "*szs*.pack")):
            names |= set(stage_actors.sarc_files(pack))
        for n in sorted(names):
            m = re.match(r"^(.+)_Room(\d+)\.szs$", n)
            if m:
                print(m.group(1), int(m.group(2)))
        return
    if not a.spawns or not a.out:
        ap.error("SPAWNS and --out are needed")
    archived = None
    if a.rooms:
        archived = {(l.split()[0], int(l.split()[1])) for l in open(a.rooms) if len(l.split()) == 2}
    skip = set(s for s in a.skip.split(",") if s)
    rooms = {}                                          # (stage, room) -> (plain, point)
    for path in a.spawns:
        for line in open(path):
            m = LINE.match(line.strip())
            if not m:
                continue
            stage, room, point, params = m.group(1), int(m.group(4)), int(m.group(3)), int(m.group(5), 16)
            if archived is not None and (stage, room) not in archived:
                continue
            if room == 63 or stage in skip or f"{stage}:{room}" in skip:
                continue                                # 63 (0x3F): no room (two entries, both stages' rooms have their own)
            plain = (params >> 12) & 0xF == 0
            have = rooms.get((stage, room))
            if have is None or (plain and not have[0]):
                rooms[(stage, room)] = (plain, point)
    visits = sorted(rooms.items())
    os.makedirs(a.out, exist_ok=True)
    n = max(1, min(a.chunks, len(visits)))
    for k in range(n):
        part = visits[k::n]                             # interleaved: each chunk a mix of stages
        warps, script, listing = [], list(BOOT), []
        for i, ((stage, room), (_, point)) in enumerate(part):
            t = 920 + i * a.visit
            warps.append(f"{t}:{stage},{point},{room},-1")
            script.append(f"{t + a.visit // 3} RLEFT {a.visit // 2}")
            listing.append(f"{t} {stage} {room} {point}")
        last = 920 + len(part) * a.visit
        with open(os.path.join(a.out, f"chunk{k}.env"), "w") as f:
            f.write(f"WWHD_DEBUG_STAGE={';'.join(warps)}\nSIXTY_FRAMES={last}\n")
        with open(os.path.join(a.out, f"chunk{k}.txt"), "w") as f:
            f.write(f"# stage survey chunk {k} of {n} (tools/shaders/survey_route.py): {len(part)} rooms\n")
            f.write("\n".join(script) + "\n")
        with open(os.path.join(a.out, f"chunk{k}.list"), "w") as f:
            f.write("\n".join(listing) + "\n")
    print(f"{len(visits)} rooms in {n} chunks of ~{len(visits) // n}, {a.visit} frames a visit "
          f"(~{920 + (len(visits) // n + 1) * a.visit} frames a chunk)", file=sys.stderr)


if __name__ == "__main__":
    main()
