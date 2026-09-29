"""Print Cemu's compile flags for one of its source files, one per line, minus the output, PCH and
LTO flags, so our own sources compile against Cemu's headers exactly as Cemu's do.

Usage: python3 tools/cemu_flags.py BUILD/compile_commands.json SOURCE_SUFFIX
(e.g. Espresso/Interpreter/PPCInterpreterFPU.cpp). Used by tools/recomp/fuzz/build.sh and
src/build.sh.
"""
import json
import shlex
import sys


def flags(db, suffix):
    for e in json.load(open(db)):
        if e["file"].endswith(suffix):
            a = shlex.split(e["command"])[1:]
            out, skip = [], 0
            for i, x in enumerate(a):
                if skip:
                    skip -= 1
                elif x in ("-o", "-c"):
                    skip = 1
                elif x == "-Xclang" and i + 1 < len(a) and a[i + 1] in ("-include-pch", "-include"):
                    skip = 3
                elif not (x.startswith("-flto") or x == "-Winvalid-pch"):
                    out.append(x)
            return out
    sys.exit(f"{suffix}: not in {db}")


if __name__ == "__main__":
    print("\n".join(flags(sys.argv[1], sys.argv[2])))
