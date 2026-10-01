# /// script
# requires-python = ">=3.10"
# dependencies = ["pyghidra==3.0.2"]
# ///
"""Print functions as Ghidra's decompiler sees them, with their callers and callees.

Usage (on the worker, after tools/ghidra/rebuild.sh):
    uv run tools/ghidra/decompile.py ADDR... [--out DIR] [--project ghidra/projects]

ADDR is a function's entry point or any address inside it (hex). Each function goes to stdout, or to
DIR/f_XXXXXXXX.c with --out: its callers, its callees, then the decompiled C. Decompiler output is
the game's code in another form: keep it on the worker (e.g. /wwhd/data/ghidra-out), never in git.
"""
import argparse
import sys
from pathlib import Path

import pyghidra

import wwhd_env


def describe(program, decomp, monitor, address):
    listing = program.getFunctionManager()
    addr = program.getAddressFactory().getDefaultAddressSpace().getAddress(address)
    func = listing.getFunctionContaining(addr)
    if func is None:
        return f"// {address:08x}: in no function\n"
    entry = func.getEntryPoint().getOffset()
    callers = sorted({f.getEntryPoint().getOffset() for f in func.getCallingFunctions(monitor)})
    callees = sorted({f.getEntryPoint().getOffset() for f in func.getCalledFunctions(monitor)})
    out = [f"// f_{entry:08X} ({func.getName()}), {func.getBody().getNumAddresses()} bytes"]
    out.append("// callers: " + " ".join(f"f_{c:08X}" for c in callers))
    out.append("// callees: " + " ".join(f"f_{c:08X}" for c in callees))
    result = decomp.decompileFunction(func, 120, monitor)
    out.append(result.getDecompiledFunction().getC() if result.decompileCompleted() else f"// decompile failed: {result.getErrorMessage()}")
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("addresses", nargs="+")
    ap.add_argument("--out")
    ap.add_argument("--project", default=wwhd_env.PROJECT_DIR)
    ap.add_argument("--program", default=wwhd_env.PROGRAM)
    args = ap.parse_args()

    wwhd_env.start()
    from ghidra.app.decompiler import DecompInterface
    from ghidra.util.task import ConsoleTaskMonitor
    project = wwhd_env.open_project(args.project)
    try:
        with pyghidra.program_context(project, args.program) as program:
            decomp = DecompInterface()
            decomp.openProgram(program)
            monitor = ConsoleTaskMonitor()
            for a in args.addresses:
                text = describe(program, decomp, monitor, int(a.removeprefix("f_").removeprefix("0x"), 16))
                if args.out:
                    Path(args.out).mkdir(parents=True, exist_ok=True)
                    name = text.split()[1]
                    (Path(args.out) / f"{name}.c").write_text(text)
                    print(f"{a} -> {Path(args.out) / (name + '.c')}")
                else:
                    sys.stdout.write(text + "\n")
    finally:
        project.close()


if __name__ == "__main__":
    main()
