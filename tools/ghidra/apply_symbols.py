# /// script
# requires-python = ">=3.10"
# dependencies = ["pyghidra==3.0.2"]
# ///
"""Apply config/US_v0/symbols.csv to the Ghidra project (idempotent).

symbols.csv is the reviewed, committed source of truth for names; the Ghidra
project is disposable. Columns: address,name,evidence. `A::B::name` puts the
symbol in namespace A::B. Addresses in .text become function names (creating
the function if needed); anything else becomes a primary data label.

Usage:
    uv run tools/ghidra/apply_symbols.py [--symbols config/US_v0/symbols.csv]
"""
import argparse
import csv
import sys

import pyghidra

import wwhd_env


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--symbols", default=wwhd_env.ROOT / "config" / "US_v0" / "symbols.csv")
    ap.add_argument("--project", default=wwhd_env.PROJECT_DIR)
    args = ap.parse_args()
    rows = list(csv.DictReader(open(args.symbols)))

    wwhd_env.start()
    from ghidra.app.cmd.function import CreateFunctionCmd
    from ghidra.app.util import NamespaceUtils
    from ghidra.program.model.symbol import SourceType

    project = wwhd_env.open_project(args.project)
    stats = {"function": 0, "label": 0, "created_function": 0, "unchanged": 0, "outside_module": 0}
    try:
        with pyghidra.program_context(project, wwhd_env.PROGRAM) as program:
            space = program.getAddressFactory().getDefaultAddressSpace()
            memory, fm = program.getMemory(), program.getFunctionManager()
            symtab = program.getSymbolTable()
            text = memory.getBlock(".text")
            monitor = pyghidra.task_monitor()
            with pyghidra.transaction(program, f"apply {args.symbols}"):
                for row in rows:
                    addr = space.getAddress(int(row["address"], 16))
                    if memory.getBlock(addr) is None:
                        stats["outside_module"] += 1
                        print(f"skip {row['address']} {row['name']}: not in any section")
                        continue
                    *path, name = row["name"].split("::")
                    ns = (NamespaceUtils.createNamespaceHierarchy("::".join(path), None, program, SourceType.USER_DEFINED)
                          if path else program.getGlobalNamespace())
                    if text.contains(addr):
                        fn = fm.getFunctionAt(addr)
                        if fn is None:
                            CreateFunctionCmd(addr).applyTo(program, monitor)
                            fn = fm.getFunctionAt(addr)
                            stats["created_function"] += 1
                        if fn.getName() == name and fn.getParentNamespace() == ns:
                            stats["unchanged"] += 1
                            continue
                        fn.setParentNamespace(ns)
                        fn.setName(name, SourceType.USER_DEFINED)
                        stats["function"] += 1
                    else:
                        existing = symtab.getPrimarySymbol(addr)
                        if existing and existing.getName() == name and existing.getParentNamespace() == ns:
                            stats["unchanged"] += 1
                            continue
                        symtab.createLabel(addr, name, ns, SourceType.USER_DEFINED).setPrimary()
                        stats["label"] += 1
            program.save(f"apply {args.symbols}", monitor)
    finally:
        project.close()
    print(", ".join(f"{k} {v}" for k, v in stats.items()))


if __name__ == "__main__":
    sys.exit(main())
