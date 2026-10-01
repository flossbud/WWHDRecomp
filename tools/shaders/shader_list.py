"""Merge captured shader sources into the shader list (docs/recompiler-design.md D20: a first start
without hitches).

Usage (on the worker):
    python3 tools/shaders/shader_list.py OUT SOURCES... [--list LIST] [--corpus INDEX_CSV] [--content DIR]

SOURCES are files a run wrote with WWHD_SHADER_SOURCES=path (src/gpu/vk/shader_list.cpp has the
format): every shader, fetch shader and pipeline the run met for the first time, with the registers
its translation read. LIST is an existing list to add to (usually config/US_v0/shader_list.txt). OUT
gets the merged list, sorted, with each program's content file: the G1 corpus (tools/shaders/corpus.py
index.csv, default /wwhd/data/g1/index.csv) says where a program was first found, and the longest
part of that path that is a file in the title's content folder (--content) is where the player's
machine will look. A shader whose program isn't in the game's files is left out (it will be
translated when it is first met).

The list holds program hashes and sizes, content file names, fetch shaders (GX2 builds them at
runtime from the vertex layout: state, not game files), register values and pipeline recipes. No
game content, so it can be committed; check that a change to it keeps it so.
"""
import argparse
import csv
import os
import sys

HEADER = """# WWHD shader list (docs/recompiler-design.md D20; src/gpu/vk/shader_list.cpp has the format).
# Everything playthroughs have translated and built, so that a first start prepares it from the
# player's own game files. Program hashes and content file names, fetch shaders (vertex layouts),
# register values and pipeline recipes: no game content. Made by tools/shaders/shader_list.py.
"""


def read(path, into, conflicts):
    programs, fetches, shaders, pipelines = into
    with open(path) as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            kind, _, rest = line.partition(" ")
            if kind == "program":
                h, size, *where = rest.split(" ", 2)
                old = programs.get(h)
                programs[h] = (int(size), where[0] if where and where[0] != "-" else (old[1] if old else None))
            elif kind == "fetch":
                h = rest.split(" ", 1)[0]
                fetches.setdefault(h, line)
            elif kind in ("vs", "ps"):
                key = rest.split(" ", 1)[0]
                if key in shaders and shaders[key] != line:
                    conflicts.append(key)
                shaders.setdefault(key, line)
            elif kind == "pipeline":
                pipelines.add(rest)
            else:
                sys.exit(f"{path}:{n}: {kind!r} lines aren't known")


def content_file(source, content):
    """The longest prefix of a G1 source path that is a file in the content folder (corpus.py names a
    program in a file "FILE#n", and a shader file inside another "FILE@offset")."""
    parts = [part.split("#")[0].split("@")[0] for part in source.split("/")]
    for k in range(len(parts), 0, -1):
        candidate = "/".join(parts[:k])
        if os.path.isfile(os.path.join(content, candidate)):
            return candidate
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("sources", nargs="+")
    ap.add_argument("--list", help="an existing list to add to")
    ap.add_argument("--corpus", default="/wwhd/data/g1/index.csv")
    ap.add_argument("--content", default="/wwhd/data/orig/0005000010143500_v0/content")
    args = ap.parse_args()

    programs, fetches, shaders, pipelines, conflicts = {}, {}, {}, set(), []
    into = (programs, fetches, shaders, pipelines)
    for path in ([args.list] if args.list and os.path.exists(args.list) else []) + args.sources:
        read(path, into, conflicts)

    corpus = {}
    with open(args.corpus) as f:
        for r in csv.DictReader(f):
            corpus[r["hash"]] = r["first_source"]
    unplaced = set()
    for h, (size, where) in programs.items():
        if where is None and h in corpus:
            where = content_file(corpus[h], args.content)
        if where is None:
            unplaced.add(h)
        programs[h] = (size, where)

    kept = {}
    for key, line in shaders.items():
        program = line.split(" ")[2]
        if program not in unplaced:
            kept[key] = line
    used = {line.split(" ")[2] for line in kept.values()}
    fetches_used = {line.split(" ")[3] for line in kept.values() if line.startswith("vs ")}

    with open(args.out, "w") as f:
        f.write(HEADER)
        for h in sorted(used):
            f.write(f"program {h} {programs[h][0]} {programs[h][1]}\n")
        for h in sorted(fetches_used & fetches.keys()):
            f.write(fetches[h] + "\n")
        for key in sorted(kept):
            f.write(kept[key] + "\n")
        for recipe in sorted(pipelines):
            f.write(f"pipeline {recipe}\n")
    files = {programs[h][1] for h in used}
    print(f"{len(kept)} shaders ({len(shaders) - len(kept)} left out: their program isn't in the game's files), {len(used)} programs "
          f"in {len(files)} content files, {len(fetches_used)} fetch shaders, {len(pipelines)} pipelines -> {args.out}")
    if fetches_used - fetches.keys():
        print(f"warning: {len(fetches_used - fetches.keys())} fetch shaders the vertex shaders name aren't in the sources", file=sys.stderr)
    if conflicts:
        print(f"{len(conflicts)} keys were met again with other register values (the first is kept; translation reads few of them)")


if __name__ == "__main__":
    main()
