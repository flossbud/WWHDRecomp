# /// script
# requires-python = ">=3.10"
# dependencies = ["pyghidra==3.0.2"]
# ///
"""Which functions name which source file: the assert strings the game kept.

Usage (on the worker, after tools/ghidra/rebuild.sh):
    uv run tools/ghidra/source_files.py OUT.tsv

WWHD kept the GameCube code's assertions: a failed one calls the game's panic function with a
source file name ("f_op_actor.cpp"), a line number and the expression. Every string in the program
that looks like a source file name, and every instruction that refers to it, gives a row:
file, function entry, the referring address, and the line number when an `li r4, N` (the line
argument) sits within a few instructions of the reference. That maps WWHD's functions to the
files of zeldaret/tww (and of sead, JSystem and the SDK), whose line numbers are close to the
decomp's. Rows are knowledge about the game's code: keep OUT on the worker.
"""
import re
import sys

import jpype
import pyghidra

import wwhd_env

NAME = re.compile(rb"[A-Za-z0-9_\-./]{1,80}\.(?:cpp|cc|c|h|hpp|inc)\0")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = open(sys.argv[1], "w")
    wwhd_env.start()
    project = wwhd_env.open_project()
    try:
        with pyghidra.program_context(project, wwhd_env.PROGRAM) as program:
            space = program.getAddressFactory().getDefaultAddressSpace()
            functions = program.getFunctionManager()
            refs = program.getReferenceManager()
            listing = program.getListing()
            memory = program.getMemory()

            def line_near(at):
                """The immediate of an `li r4, N` (the assert's line argument) within 8
                instructions of `at`, after it first, then before it; or empty."""
                for step in ("getNext", "getPrevious"):
                    insn = listing.getInstructionAt(at)
                    for _ in range(8):
                        insn = getattr(insn, step)() if insn is not None else None
                        if insn is None:
                            break
                        m = re.fullmatch(r"li r4,(0x[0-9a-f]+|-?\d+)", insn.toString())
                        if m:
                            return str(int(m.group(1), 0))
                        if insn.getMnemonicString() in ("bl", "b", "blr", "bctr", "bctrl"):
                            break
                return ""

            rows = 0
            for block in memory.getBlocks():
                if not block.isInitialized() or block.isExecute():
                    continue
                size = int(block.getSize())
                if size > 64 << 20:
                    continue
                # a Java array: JPype copies a Python bytearray, so getBytes would fill the copy
                buf = jpype.JArray(jpype.JByte)(size)
                block.getBytes(block.getStart(), buf)
                data = bytes(b & 0xFF for b in buf)
                for m in NAME.finditer(data):
                    # the string must start at a string boundary (NUL or the block's start before it)
                    if m.start() > 0 and data[m.start() - 1] != 0:
                        continue
                    name = m.group(0)[:-1].decode()
                    addr = block.getStart().add(m.start())
                    for r in refs.getReferencesTo(addr):
                        src = r.getFromAddress()
                        f = functions.getFunctionContaining(src)
                        entry = f"{f.getEntryPoint().getOffset():08X}" if f else ""
                        out.write(f"{name}\t{entry}\t{src.getOffset():08X}\t{line_near(src)}\t{addr.getOffset():08X}\n")
                        rows += 1
            print(f"{rows} references to source file names -> {sys.argv[1]}")
    finally:
        project.close()
        out.close()


if __name__ == "__main__":
    main()
