# /// script
# requires-python = ">=3.10"
# dependencies = ["pyghidra==3.0.2"]
# ///
"""Look things up in the Ghidra project: who refers to an address, and what words are stored where.

Usage (on the worker, after tools/ghidra/rebuild.sh):
    uv run tools/ghidra/lookup.py refs ADDR...         # every reference to ADDR (code and data)
    uv run tools/ghidra/lookup.py words ADDR COUNT     # COUNT 32-bit words from ADDR, with the
                                                        # function each one points into, if any
    uv run tools/ghidra/lookup.py find VALUE...        # where memory holds VALUE as a 32-bit word
                                                        # (pointers Ghidra didn't mark: vtables)

For example, the vtable holding a method: `refs` on its entry gives the data word that points to
it; `words` around that word lists its neighbours. Addresses are hex. Output is about the game's
code: keep it on the worker.
"""
import sys

import pyghidra

import wwhd_env


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("refs", "words", "find"):
        sys.exit(__doc__)
    wwhd_env.start()
    project = wwhd_env.open_project()
    try:
        with pyghidra.program_context(project, wwhd_env.PROGRAM) as program:
            space = program.getAddressFactory().getDefaultAddressSpace()
            functions = program.getFunctionManager()
            refs = program.getReferenceManager()
            memory = program.getMemory()

            def name(addr):
                f = functions.getFunctionContaining(space.getAddress(addr))
                return f"f_{f.getEntryPoint().getOffset():08X}+{addr - f.getEntryPoint().getOffset():#x}" if f else ""

            if sys.argv[1] == "refs":
                for a in sys.argv[2:]:
                    addr = int(a.removeprefix("f_"), 16)
                    for r in refs.getReferencesTo(space.getAddress(addr)):
                        src = r.getFromAddress().getOffset()
                        print(f"{addr:08x} <- {src:08x} {r.getReferenceType()} {name(src)}")
            elif sys.argv[1] == "find":
                for a in sys.argv[2:]:
                    value = int(a.removeprefix("f_"), 16)
                    pattern = bytes(value.to_bytes(4, "big"))
                    for block in memory.getBlocks():
                        if not block.isInitialized():
                            continue
                        at = block.getStart()
                        while at is not None:
                            at = memory.findBytes(at, block.getEnd(), pattern, None, True, None)
                            if at is None:
                                break
                            print(f"{value:08x} at {at.getOffset():08x} ({block.getName()}) {name(at.getOffset())}")
                            at = at.add(1)
            else:
                start, count = int(sys.argv[2], 16), int(sys.argv[3], 0)
                for i in range(count):
                    a = start + 4 * i
                    try:
                        w = memory.getInt(space.getAddress(a)) & 0xFFFFFFFF
                    except Exception:
                        print(f"{a:08x}: (not in memory)")
                        continue
                    print(f"{a:08x} +{4 * i:#05x}: {w:08x} {name(w)}")
    finally:
        project.close()


if __name__ == "__main__":
    main()
