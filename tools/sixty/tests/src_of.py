"""src_of.py DIR ADDR...: for each store in the disassembly DIR (tools/ghidra/disasm.py output), the last
instruction before it (within 40) that writes its source register: where a step rule goes (`*h@f`
on an fadds, `spliti` on an addi, `split@r` on an add), as tools/sixty/actor_rmw.py lists stores."""
import glob
import re
import sys

d = sys.argv[1]
lines = {}
for f in glob.glob(d + "/f_*.s"):
    L = [l.rstrip("\n") for l in open(f) if re.match(r"[0-9a-f]{8}  ", l)]
    for i, l in enumerate(L):
        lines[l[:8]] = (L, i)
for a in sys.argv[2:]:
    L, i = lines[a.lower()]
    st = L[i][10:]
    src = re.match(r"\S+ ([rf]\d+),", st).group(1)
    for j in range(i - 1, max(0, i - 40), -1):
        m = re.match(r"(\S+) ([rf]\d+),", L[j][10:])
        if m and m.group(2) == src and not m.group(1).startswith("st"):
            print(a, st, "<-", L[j])
            break
