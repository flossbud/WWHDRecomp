# /// script
# requires-python = ">=3.10"
# dependencies = ["pyghidra==3.0.2"]
# ///
"""Find strings in the program and the functions that refer to them.

Usage (on the worker, after tools/ghidra/rebuild.sh):
    uv run tools/ghidra/strings.py REGEX [--max N]

Every NUL-terminated ASCII string (4 characters or more) in the program's initialized data that
REGEX (Python, searched) matches is printed with its address and the functions whose code refers to
it. For finding code by the names it uses (resource files, profiler labels, thread names). The
output is about the game's code: keep it on the worker.
"""
import argparse
import re

import jpype
import pyghidra

import wwhd_env

STRING = re.compile(rb"[\x20-\x7e]{4,}\0")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("regex")
    ap.add_argument("--max", type=int, default=200)
    args = ap.parse_args()
    want = re.compile(args.regex)
    wwhd_env.start()
    project = wwhd_env.open_project()
    try:
        with pyghidra.program_context(project, wwhd_env.PROGRAM) as program:
            functions = program.getFunctionManager()
            refs = program.getReferenceManager()
            shown = 0
            for block in program.getMemory().getBlocks():
                if not block.isInitialized() or block.isExecute() or block.getSize() > 64 << 20:
                    continue
                buf = jpype.JArray(jpype.JByte)(int(block.getSize()))
                block.getBytes(block.getStart(), buf)
                data = bytes(b & 0xFF for b in buf)
                for m in STRING.finditer(data):
                    if m.start() > 0 and data[m.start() - 1] != 0:
                        continue
                    text = m.group(0)[:-1].decode()
                    if not want.search(text):
                        continue
                    addr = block.getStart().add(m.start())
                    users = sorted({f"f_{f.getEntryPoint().getOffset():08X}"
                                    for r in refs.getReferencesTo(addr)
                                    if (f := functions.getFunctionContaining(r.getFromAddress())) is not None})
                    print(f"{addr.getOffset():08x}  {text!r}  {' '.join(users)}")
                    shown += 1
                    if shown >= args.max:
                        return
    finally:
        project.close()


if __name__ == "__main__":
    main()
