"""Print the archive members a link needs, in the order another link pulled them in.

Usage: python3 src/link_order.py REFERENCE.map TARGET.map [MEMBER=OBJECT...]
Prints "archive<TAB>member" for every member of TARGET's link (the GNU ld map's "Archive member
included" section), ordered as REFERENCE's link included them; members REFERENCE never used come
last. MEMBER=OBJECT puts our OBJECT where REFERENCE had Cemu's MEMBER (printed as "-<TAB>OBJECT"):
how our forks of Cemu's sources (src/os/snd_core) take their place. Static constructors run in link order, and Cemu's SysAllocators take their slots in guest
memory in constructor order, so wwhd-null must be linked in wwhd's member order to get the same
guest memory layout (src/build.sh). Archive names are compared without the "_wwhd" suffix of the
copies build.sh makes (without Latte, without the objects our forks replace).
"""
import re
import sys

MEMBER = re.compile(r"^(\S+\.a)\((.+)\)$")


def members(path):
    out = []
    with open(path) as f:
        for line in f:
            if line.startswith("Discarded input sections"):
                break
            m = MEMBER.match(line.rstrip("\n"))
            if m:
                out.append((m.group(1), m.group(2)))
    return out


def key(archive, member):
    return (archive.replace("_wwhd.a", ".a").rsplit("/", 1)[-1], member)


reference = members(sys.argv[1])
ref = {key(a, m): i for i, (a, m) in enumerate(reference)}
replace = dict(arg.split("=", 1) for arg in sys.argv[3:])
entries = [(ref.get(key(a, m), len(ref)), a, m) for a, m in members(sys.argv[2])]
for i, (a, m) in enumerate(reference):
    if m in replace:
        entries.append((i, "-", replace.pop(m)))
if replace:
    sys.exit(f"not in {sys.argv[1]}: {', '.join(replace)}")
entries.sort(key=lambda e: e[0])
for _, a, m in entries:
    print(f"{a}\t{m}")
