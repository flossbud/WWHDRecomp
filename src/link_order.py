"""The order wwhd-null links archive members in (src/CMakeLists.txt, WWHD_LINK_ORDER).

Static constructors run in link order, and Cemu's SysAllocators take their slots in guest memory in
constructor order. So wwhd-null links every archive member it needs explicitly, in the order the
reference's link (Cemu's own) pulled them in; otherwise host-side addresses in the 0x0E000000 area
differ from the reference's and no trace compares.

Usage:
  python3 src/link_order.py save REFERENCE.map > src/link_order.txt
      the members a GNU ld link pulled from archives (its -Wl,-Map), in order, as
      "archive<TAB>member" with archives by file name: from a link of Cemu's own (Cemu_release, or
      Cemu's link line with our frontend), whenever the pinned Cemu or its vcpkg libraries change
  python3 src/link_order.py link ORDER.txt LINK.map LINK_DIR MEMBERS_DIR AR RSP
      the members LINK (run in LINK_DIR) pulled from archives, in ORDER's order (those it doesn't
      list come last, as LINK pulled them), extracted with AR into MEMBERS_DIR/ARCHIVE/ and listed
      in RSP, a response file for the ordered link

Our forks of Cemu's sources compile into Cemu's archives under their originals' names, so they take
their originals' places.
"""
import os
import re
import shutil
import subprocess
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


def save(reference):
    for archive, member in members(reference):
        print(f"{os.path.basename(archive)}\t{member}")


def link(order_txt, link_map, link_dir, members_dir, ar, rsp):
    order = {}
    with open(order_txt) as f:
        for line in f:
            if line.strip() and not line.startswith("#"):
                order.setdefault(tuple(line.rstrip("\n").split("\t")), len(order))
    pulled = [(os.path.join(link_dir, a), m) for a, m in members(link_map)]
    if not pulled:
        sys.exit(f"{link_map}: no archive members (not a GNU ld map?)")
    ranked = sorted(range(len(pulled)), key=lambda i: (order.get((os.path.basename(pulled[i][0]), pulled[i][1]), len(order)), i))
    shutil.rmtree(members_dir, ignore_errors=True)
    dirs = {}
    for archive in dict.fromkeys(a for a, _ in pulled):
        names = subprocess.run([ar, "t", archive], check=True, capture_output=True, text=True).stdout.split()
        wanted = {m for a, m in pulled if a == archive}
        twice = sorted(n for n in wanted if names.count(n) > 1)
        if twice:
            sys.exit(f"{archive}: members named alike, which extracting can't tell apart: {', '.join(twice)}")
        d = os.path.join(members_dir, f"{len(dirs):03d}_{os.path.basename(archive)}")
        os.makedirs(d)
        subprocess.run([ar, "x", os.path.abspath(archive)], check=True, cwd=d)
        dirs[archive] = d
    unlisted = 0
    with open(rsp + ".tmp", "w") as out:
        for i in ranked:
            archive, member = pulled[i]
            unlisted += (os.path.basename(archive), member) not in order
            out.write(os.path.join(dirs[archive], member) + "\n")
    os.replace(rsp + ".tmp", rsp)
    print(f"link_order: {len(pulled)} archive members from {len(dirs)} archives, {unlisted} not in {os.path.basename(order_txt)} (last)")


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "save":
        save(sys.argv[2])
    elif len(sys.argv) == 8 and sys.argv[1] == "link":
        link(*sys.argv[2:])
    else:
        sys.exit(__doc__)
