"""Name WWHD's actor process numbers after the GameCube decomp's profiles (docs/recompiler-design.md D21).

Usage:
    python3 tools/sixty/actor_names.py TWW_DIR PROFILES.tsv SOURCE_FILES.tsv > NAMES.tsv

TWW_DIR is a checkout of zeldaret/tww. PROFILES.tsv lists WWHD's actor profiles (address, process
name, size, draw priority, method table, status, group, then the five methods: the words around
each reference to g_fopAc_Method, 0x101F3088), SOURCE_FILES.tsv is tools/ghidra/source_files.py's
output. WWHD numbers its process names differently (Link is 168, 169 on the GameCube), so:
  1. a profile whose methods lie among the functions that assert a source file d_a_X.cpp (or an .inc
     it includes; headers don't count) is that file's profile (by order, when it defines several);
  2. between two profiles named that way, the others follow the GameCube's order of process names,
     when both sides have the same number of profiles in between.
Prints number, name (the decomp's fpcNm_ name), the source file and how it was found. The output is
names and numbers only.
"""
import bisect
import collections
import pathlib
import re
import sys


def gc_profiles(tww):
    """[(process number, fpcNm name, file)] for every actor profile the decomp defines."""
    header = (tww / "include/f_pc/f_pc_name.h").read_text()
    body = header[header.index("{", header.index("enum")):]
    numbers, v = {}, -1
    for name, val in re.findall(r"\b(fpcNm_\w+_e)\s*(?:=\s*(0x[0-9A-Fa-f]+|\d+))?\s*,", body):
        v = int(val, 0) if val else v + 1
        numbers[name] = v
    out = []
    for path in sorted((tww / "src").rglob("*.cpp")):
        text = path.read_text(errors="replace")
        for m in re.finditer(r"actor_process_profile_definition2?\s+g_profile_\w+\s*=\s*\{(.*?)\};", text, re.S):
            proc = re.search(r"\b(fpcNm_\w+_e)\b", m.group(1))
            if proc and "g_fopAc_Method" in m.group(1) and proc.group(1) in numbers:
                out.append((numbers[proc.group(1)], proc.group(1), path.name))
    return sorted(set(out))


def main():
    tww, profiles, sources = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
    gc = gc_profiles(tww)
    by_file = collections.defaultdict(list)
    for num, name, f in gc:
        by_file[f].append((num, name))

    # an .inc file belongs to the .cpp that includes it; headers' inline functions are everywhere
    includer = {}
    for path in (tww / "src").rglob("*.cpp"):
        for inc in re.findall(r'#include\s+"[^"]*?([\w.]+\.inc)"', path.read_text(errors="replace")):
            includer[inc] = path.name
    asserted = []
    for line in open(sources):
        f, entry = line.split("\t")[:2]
        f = includer.get(f, f)
        if entry and f.endswith(".cpp"):
            asserted.append((int(entry, 16), f))
    asserted.sort()
    keys = [a for a, _ in asserted]

    def file_of(addr):
        i = bisect.bisect_right(keys, addr) - 1
        if i < 0:
            return None
        lo = asserted[i]
        if lo[0] == addr:
            return lo[1]
        hi = asserted[i + 1] if i + 1 < len(asserted) else None
        return lo[1] if hi and hi[1] == lo[1] else None

    wwhd = []
    for line in open(profiles):
        p = line.rstrip("\n").split("\t")
        methods = [int(x, 16) for x in p[7:12] if int(x, 16)]
        files = {file_of(m) for m in methods} - {None}
        files = {f for f in files if f.endswith(".cpp") and f.startswith("d_a_")}
        wwhd.append((int(p[1]), files.pop() if len(files) == 1 else None))
    wwhd.sort()

    named = {}                                   # WWHD number -> (gc number, gc name, file, how)
    used = collections.Counter()
    for num, f in wwhd:
        if f and by_file.get(f):
            k = used[f]
            if k < len(by_file[f]):
                gnum, gname = by_file[f][k]
                named[num] = (gnum, gname, f, "assert")
                used[f] += 1
    # between anchors, by order
    gc_nums = [g[0] for g in gc]
    anchors = sorted(named)
    for a, b in zip(anchors, anchors[1:]):
        ours = [n for n, _ in wwhd if a < n < b and n not in named]
        ga, gb = named[a][0], named[b][0]
        theirs = [g for g in gc if ga < g[0] < gb]
        if ours and len(ours) == len(theirs):
            for n, (gnum, gname, f) in zip(ours, theirs):
                named[n] = (gnum, gname, f, "order")
    print("# WWHD process number, GameCube name, file, how it was matched (tools/sixty/actor_names.py)")
    for num, _ in wwhd:
        if num in named:
            gnum, gname, f, how = named[num]
            print(f"{num}\t{gname.removeprefix('fpcNm_').removesuffix('_e')}\t{f}\t{how} (GC {gnum})")
        else:
            print(f"{num}\t?\t\tunmatched")
    print(f"# {len(named)} of {len(wwhd)} named", file=sys.stderr)


if __name__ == "__main__":
    main()
