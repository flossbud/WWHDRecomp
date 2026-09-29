"""Print the archive members a link needs, in the order another link pulled them in.

Usage: python3 src/link_order.py REFERENCE.map TARGET.map
Prints "archive<TAB>member" for every member of TARGET's link (the GNU ld map's "Archive member
included" section), ordered as REFERENCE's link included them; members REFERENCE never used come
last. Static constructors run in link order, and Cemu's SysAllocators take their slots in guest
memory in constructor order, so wwhd-null must be linked in wwhd's member order to get the same
guest memory layout (src/build.sh). Archive names are compared without a "_nolatte" suffix.
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
    return (archive.replace("_nolatte.a", ".a").rsplit("/", 1)[-1], member)


ref = {key(a, m): i for i, (a, m) in enumerate(members(sys.argv[1]))}
target = members(sys.argv[2])
target.sort(key=lambda am: ref.get(key(*am), len(ref)))
for a, m in target:
    print(f"{a}\t{m}")
