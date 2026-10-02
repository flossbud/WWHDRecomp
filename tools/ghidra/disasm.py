# /// script
# requires-python = ">=3.10"
# dependencies = ["pyghidra==3.0.2"]
# ///
"""Print functions as instructions, one per line, with the targets of calls named.

Usage (on the worker, after tools/ghidra/rebuild.sh):
    uv run tools/ghidra/disasm.py ADDR... [--out DIR]

ADDR is a function's entry or any address inside it (hex). Each line is "address  mnemonic operands",
calls with "-> f_XXXXXXXX". For placing per-instruction rules (config/US_v0/tick_rules.txt, D21) and
reading what the decompiler hides. The output is the game's code in another form: keep it on the
worker (e.g. /wwhd/data/ghidra-out), never in git.
"""
import argparse
import sys
from pathlib import Path

import pyghidra

import wwhd_env


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("addresses", nargs="+")
    ap.add_argument("--out")
    args = ap.parse_args()
    wwhd_env.start()
    project = wwhd_env.open_project()
    try:
        with pyghidra.program_context(project, wwhd_env.PROGRAM) as program:
            space = program.getAddressFactory().getDefaultAddressSpace()
            functions = program.getFunctionManager()
            listing = program.getListing()
            for a in args.addresses:
                ea = int(a.removeprefix("f_").removeprefix("0x"), 16)
                func = functions.getFunctionContaining(space.getAddress(ea))
                if func is None:
                    print(f"// {ea:08x}: in no function")
                    continue
                entry = func.getEntryPoint().getOffset()
                lines = [f"// f_{entry:08X}"]
                for insn in listing.getInstructions(func.getBody(), True):
                    text = f"{insn.getAddress().getOffset():08x}  {insn}"
                    for ref in insn.getReferencesFrom():
                        if ref.getReferenceType().isCall():
                            callee = functions.getFunctionAt(ref.getToAddress())
                            if callee is not None:
                                text += f"  -> f_{callee.getEntryPoint().getOffset():08X}"
                    lines.append(text)
                text = "\n".join(lines) + "\n"
                if args.out:
                    Path(args.out).mkdir(parents=True, exist_ok=True)
                    (Path(args.out) / f"f_{entry:08X}.s").write_text(text)
                    print(f"{a} -> {Path(args.out) / f'f_{entry:08X}.s'}")
                else:
                    sys.stdout.write(text)
    finally:
        project.close()


if __name__ == "__main__":
    main()
