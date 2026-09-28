# /// script
# requires-python = ">=3.10"
# dependencies = ["pyghidra==3.0.2"]
# ///
"""Teach Ghidra the GHS switch tables in config/US_v0/jump_tables.csv (idempotent).

For each switch: disassemble the table of `b` instructions, add COMPUTED_JUMP
references from the bctr to every table entry, delete functions that analysis
or seeding wrongly started at a table entry or case label, recompute the
owning function's body, and write a decompiler jump-table override so the
switch decompiles as a switch.

Usage (after tools/jump_tables.py):
    uv run tools/ghidra/apply_jump_tables.py
"""
import argparse
import csv
import sys

import pyghidra

import wwhd_env


def load(path):
    rows = []
    for r in csv.DictReader(open(path)):
        table, count = int(r["table"], 16), int(r["count"])
        rows.append((int(r["bctr"], 16), [table + 4 * i for i in range(count)],
                     [int(t, 16) for t in r["targets"].split()]))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tables", default=wwhd_env.ROOT / "config" / "US_v0" / "jump_tables.csv")
    ap.add_argument("--project", default=wwhd_env.PROJECT_DIR)
    args = ap.parse_args()
    switches = load(args.tables)

    wwhd_env.start()
    from java.util import ArrayList
    from ghidra.app.cmd.disassemble import DisassembleCommand
    from ghidra.app.cmd.function import CreateFunctionCmd
    from ghidra.program.model.address import AddressSet
    from ghidra.program.model.pcode import JumpTable
    from ghidra.program.model.symbol import RefType, SourceType

    project = wwhd_env.open_project(args.project)
    stats = dict(references=0, removed_functions=0, kept_named=0, owners_fixed=0, overrides=0, no_owner=0)
    try:
        with pyghidra.program_context(project, wwhd_env.PROGRAM) as program:
            space = program.getAddressFactory().getDefaultAddressSpace()
            fm, listing, refs = program.getFunctionManager(), program.getListing(), program.getReferenceManager()
            monitor = pyghidra.task_monitor()
            A = space.getAddress
            with pyghidra.transaction(program, "apply jump tables"):
                # 1. No function may start at a table entry or a case label.
                not_entries = {a for _b, entries, targets in switches for a in entries + targets}
                for a in sorted(not_entries):
                    fn = fm.getFunctionAt(A(a))
                    if fn is None:
                        continue
                    if fn.getSymbol().getSource() == SourceType.USER_DEFINED:
                        stats["kept_named"] += 1
                        print(f"keep {a:08X} {fn.getName()}: named in symbols.csv but is a switch case")
                        continue
                    fm.removeFunction(A(a))
                    stats["removed_functions"] += 1
                # 2. Tables are code, and the bctr flows to every entry. Each entry is a
                #    `b` with no fallthrough, so every one must be disassembled explicitly.
                todo = AddressSet()
                for _bctr, entries, _targets in switches:
                    for e in entries:
                        if listing.getInstructionAt(A(e)) is None:
                            todo.add(A(e))
                if not todo.isEmpty():
                    DisassembleCommand(todo, None, True).applyTo(program, monitor)
                for bctr, entries, _targets in switches:
                    existing = {r.getToAddress().getOffset() for r in refs.getReferencesFrom(A(bctr))}
                    for e in entries:
                        if e not in existing:
                            refs.addMemoryReference(A(bctr), A(e), RefType.COMPUTED_JUMP, SourceType.USER_DEFINED, 0)
                            stats["references"] += 1
                # 3. Recompute owner bodies and tell the decompiler about the switch.
                for bctr, entries, _targets in switches:
                    owner = fm.getFunctionContaining(A(bctr))
                    if owner is None:
                        stats["no_owner"] += 1
                        continue
                    if CreateFunctionCmd.fixupFunctionBody(program, owner, monitor):
                        stats["owners_fixed"] += 1
                    dests = ArrayList()
                    for e in entries:
                        dests.add(A(e))
                    JumpTable(A(bctr), dests, True).writeOverride(owner)
                    stats["overrides"] += 1
            program.save("apply jump tables", monitor)
    finally:
        project.close()
    print(", ".join(f"{k} {v}" for k, v in stats.items()))


if __name__ == "__main__":
    sys.exit(main())
