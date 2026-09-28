# /// script
# requires-python = ">=3.10"
# dependencies = ["pyghidra==3.0.2"]
# ///
"""Create functions Ghidra's auto-analysis missed, using the RPX's own relocations.

Many small leaf functions (vtable slots, callback tables) are reached only
through pointers: ADDR32 words in .data/.rodata, or lis/addi pairs
(ADDR16_HA/LO) that build a callback address in code. Ghidra leaves them as
undefined bytes. Every such target that lands in .text outside an existing
function is disassembled and made a function; then auto-analysis runs again.
Switch tables and case labels from config/US_v0/jump_tables.csv are excluded:
the lis/addic that builds a GHS branch-table address carries a relocation too.
Safe to re-run: targets already covered by a function are skipped.

Usage (after tools/ghidra/import.sh):
    uv run tools/ghidra/seed_functions.py [--rpx orig/.../cking.rpx]
"""
import argparse
import csv
import sys

import pyghidra

import wwhd_env

ROOT = wwhd_env.ROOT
Rpx = wwhd_env.load_tool("rpx_info").Rpx

ADDRESS_RELOCS = {1, 4, 6}  # ADDR32, ADDR16_LO, ADDR16_HA
DEFAULT_RPX = ROOT / "orig" / "0005000010143500_v0" / "code" / "cking.rpx"


def switch_addresses(path):
    """Table entries and case labels: code, but never a function entry."""
    out = set()
    for r in csv.DictReader(open(path)):
        table = int(r["table"], 16)
        out.update(table + 4 * i for i in range(int(r["count"])))
        out.update(int(t, 16) for t in r["targets"].split())
    return out


def code_pointers(rpx_path):
    rpx = Rpx(rpx_path)
    text = next(s for i, s in enumerate(rpx.sh) if rpx.name(i) == ".text")
    lo, hi = text[3], text[3] + rpx.size(rpx.sh.index(text))
    return sorted({t for _sec, _off, typ, t in rpx.relocations()
                   if typ in ADDRESS_RELOCS and lo <= t < hi and t % 4 == 0})


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rpx", default=DEFAULT_RPX)
    ap.add_argument("--tables", default=ROOT / "config" / "US_v0" / "jump_tables.csv")
    ap.add_argument("--project", default=wwhd_env.PROJECT_DIR)
    args = ap.parse_args()
    switch = switch_addresses(args.tables)
    all_targets = code_pointers(args.rpx)
    targets = [t for t in all_targets if t not in switch]
    print(f"{len(all_targets)} distinct relocation targets in .text, "
          f"{len(all_targets) - len(targets)} are switch tables/cases (skipped)")

    wwhd_env.start()
    from ghidra.app.cmd.disassemble import DisassembleCommand
    from ghidra.app.cmd.function import CreateFunctionCmd

    project = wwhd_env.open_project(args.project)
    try:
        with pyghidra.program_context(project, wwhd_env.PROGRAM) as program:
            space = program.getAddressFactory().getDefaultAddressSpace()
            fm, listing = program.getFunctionManager(), program.getListing()
            monitor = pyghidra.task_monitor()
            created = inside = failed = 0
            with pyghidra.transaction(program, "seed functions from relocations"):
                for t in targets:
                    addr = space.getAddress(t)
                    fn = fm.getFunctionContaining(addr)
                    if fn is not None:
                        inside += fn.getEntryPoint() != addr
                        continue
                    if listing.getInstructionAt(addr) is None:
                        DisassembleCommand(addr, None, True).applyTo(program, monitor)
                    if CreateFunctionCmd(addr).applyTo(program, monitor):
                        created += 1
                    else:
                        failed += 1
            print(f"created {created}, failed {failed}, pointer into existing function body {inside}")
            print("re-running auto-analysis ...")
            log = pyghidra.analyze(program, monitor)
            program.save("seed functions from relocations", monitor)
            print(f"done; analysis log {len(log.splitlines())} lines")
    finally:
        project.close()


if __name__ == "__main__":
    sys.exit(main())
