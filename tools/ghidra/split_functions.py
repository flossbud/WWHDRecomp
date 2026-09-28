# /// script
# requires-python = ">=3.10"
# dependencies = ["pyghidra==3.0.2"]
# ///
"""Split non-contiguous function bodies into separate functions (idempotent).

GHS emits every function as one contiguous block. When Ghidra gives a function
extra, detached address ranges, each of those is another function it reached
by a tail call it didn't recognise:
  * a switch whose `b case_N` entries jump to other functions' prologues
  * a jump through a .rodata table of function pointers, followed as a
    computed jump
  * `b` to a lone `blr` elsewhere (an empty function)
A recompiler needs these as their own functions: code in one function must
never branch into the middle of another.

For each such function: shrink its body to [entry, end of the range holding
the entry], and make every other piece a function: each detached range, and
any code in the entry's range *before* the entry (a GHS function starts at its
entry, so that is an adjacent function it tail-calls). The parent body is not
recomputed afterwards: Ghidra would pull pointer-table targets back in through
their COMPUTED_JUMP references.

A detached range can hold several back-to-back functions, and a new function
only claims the first. So each pass also fills gaps: walking uncovered .text,
a function is started at every word where GHS could start one (see
`plausible_entry`). And any address-taken code (a relocation target, switch
tables excluded) that sits inside a body is cut out as its own function: e.g.
the pure-virtual stub that follows a no-return call with no terminator between.
A body may also extend *before* its entry, contiguously:
  * loop rotation: `b test` directly before the body jumps to the loop test,
    and Ghidra made that `b` a thunk and the test the function. The function
    really starts at the `b`: merge the two.
  * otherwise the leading code is another function that this one tail-calls
    (it has its own prologue): split it off.
Finally, a function whose entry isn't an instruction (Ghidra had typed the
bytes as data, so the body is one byte) is re-disassembled; if the bytes are
padding or not code at all, the function is deleted.
Passes repeat until nothing changes.

Usage:
    uv run tools/ghidra/split_functions.py
"""
import argparse
import csv
import sys

import pyghidra

import wwhd_env

MAX_PASSES = 10


def is_terminator(w):
    return (w >> 26 == 18 and w & 1 == 0) or w in (0x4E800020, 0x4E800420)  # b, blr, bctr


def is_prologue(w):
    return w == 0x7C0802A6 or w >> 16 == 0x9421  # mflr r0 / stwu r1,-N(r1)


def plausible_entry(memory, a, address_taken):
    """GHS functions are contiguous, so one can only begin after a terminator or
    padding (zero / nop), or where the code itself says so (prologue, or its
    address is taken)."""
    off = a.getOffset()
    if off % 4:
        return False
    w = memory.getInt(a) & 0xFFFFFFFF
    prev = memory.getInt(a.subtract(4)) & 0xFFFFFFFF
    return w not in (0, 0x60000000) and (is_terminator(prev) or prev in (0, 0x60000000) or is_prologue(w) or off in address_taken)


def disassembles(program, a, monitor):
    """True if there is (or can be made) an instruction at a. Data words such as the
    0x00400000 markers in the GHS save/restore helper block are not code."""
    listing = program.getListing()
    if listing.getInstructionAt(a) is None and listing.getDefinedDataContaining(a) is None:
        DisassembleCommand(a, None, True).applyTo(program, monitor)
    return listing.getInstructionAt(a) is not None


def fill_gaps(program, fm, text, address_taken, monitor):
    """Create functions over uncovered .text, walking each gap word by word so
    one pass fills a gap holding several functions. Returns functions created."""
    memory = program.getMemory()
    covered = AddressSet()
    for fn in fm.getFunctions(True):
        if not fn.isExternal():
            covered.add(fn.getBody())
    gaps = AddressSet(text.getStart(), text.getEnd()).subtract(covered)
    created = 0
    for r in list(gaps.getAddressRanges()):
        a = r.getMinAddress().add((4 - r.getMinAddress().getOffset() % 4) % 4)
        while a <= r.getMaxAddress():
            nxt = a.add(4)
            if (fm.getFunctionContaining(a) is None and plausible_entry(memory, a, address_taken)
                    and disassembles(program, a, monitor) and CreateFunctionCmd(a).applyTo(program, monitor)):
                created += 1
                end = fm.getFunctionAt(a).getBody().getMaxAddress().getOffset() + 1
                end += (4 - end % 4) % 4
                if end > nxt.getOffset():
                    nxt = a.getNewAddress(end)
            a = nxt
    return created


def cut_address_taken(program, fm, address_taken, monitor):
    """Make every address-taken location inside a function body its own function."""
    space = program.getAddressFactory().getDefaultAddressSpace()
    cut = 0
    for off in sorted(address_taken):
        a = space.getAddress(off)
        fn = fm.getFunctionContaining(a)
        if fn is None or fn.getEntryPoint() == a:
            continue
        fn.setBody(AddressSet(fn.getEntryPoint(), a.subtract(1)))
        if CreateFunctionCmd(a).applyTo(program, monitor):
            cut += 1
    return cut


def address_taken_code(rpx_path, tables_path):
    """Relocation targets in .text, minus switch tables and case labels."""
    rpx = wwhd_env.load_tool("rpx_info").Rpx(rpx_path)
    i = next(i for i in range(len(rpx.sh)) if rpx.name(i) == ".text")
    lo, hi = rpx.sh[i][3], rpx.sh[i][3] + rpx.size(i)
    switch = set()
    for r in csv.DictReader(open(tables_path)):
        table = int(r["table"], 16)
        switch.update(table + 4 * k for k in range(int(r["count"])))
        switch.update(int(t, 16) for t in r["targets"].split())
    return {t for _s, _o, typ, t in rpx.relocations()
            if typ in (1, 4, 6) and lo <= t < hi and t % 4 == 0 and t not in switch}


def fix_early_bodies(program, fm, monitor):
    """Functions whose body starts before their entry: re-root or split (see module doc).
    Returns (merged, split)."""
    listing = program.getListing()
    merged = split = 0
    for fn in [f for f in fm.getFunctions(True)
               if not f.isExternal() and f.getBody().getMinAddress() < f.getEntryPoint()]:
        entry, body = fn.getEntryPoint(), fn.getBody()
        start = body.getMinAddress()
        before = start.subtract(4)
        thunk = fm.getFunctionAt(before)
        insn = listing.getInstructionAt(before)
        is_rotation = (thunk is not None and insn is not None and insn.getMnemonicString() == "b"
                       and insn.getFlows() and insn.getFlows()[0] == entry)
        if is_rotation:
            # Recreating the function would make Ghidra see a lone `b` and build
            # the thunk again, so un-thunk the existing one and grow its body.
            # Un-thunk first: removing a thunk's target deletes the thunk too.
            end = body.getMaxAddress()
            if thunk.isThunk():
                thunk.setThunkedFunction(None)
            fm.removeFunction(entry)
            thunk.setBody(AddressSet(before, end))
            merged += 1
        else:
            fn.setBody(AddressSet(entry, body.getMaxAddress()))
            if fm.getFunctionAt(start) is None and CreateFunctionCmd(start).applyTo(program, monitor):
                split += 1
    return merged, split


def repair_undisassembled(program, fm, text, monitor):
    """Functions in .text with no instruction at the entry. Returns (repaired, removed)."""
    listing, memory = program.getListing(), program.getMemory()
    repaired = removed = 0
    for fn in [f for f in fm.getFunctions(text.getStart(), True)
               if text.contains(f.getEntryPoint()) and listing.getInstructionAt(f.getEntryPoint()) is None]:
        entry = fn.getEntryPoint()
        if memory.getInt(entry) & 0xFFFFFFFF not in (0, 0x60000000):
            data = listing.getDefinedDataContaining(entry)
            if data is not None:
                listing.clearCodeUnits(data.getMinAddress(), data.getMaxAddress(), False)
            DisassembleCommand(entry, None, True).applyTo(program, monitor)
        if listing.getInstructionAt(entry) is not None:
            CreateFunctionCmd.fixupFunctionBody(program, fn, monitor)
            repaired += 1
        else:
            fm.removeFunction(entry)
            removed += 1
    return repaired, removed


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--project", default=wwhd_env.PROJECT_DIR)
    ap.add_argument("--rpx", default=wwhd_env.ROOT / "orig" / "0005000010143500_v0" / "code" / "cking.rpx")
    ap.add_argument("--tables", default=wwhd_env.ROOT / "config" / "US_v0" / "jump_tables.csv")
    args = ap.parse_args()
    address_taken = address_taken_code(args.rpx, args.tables)

    wwhd_env.start()
    global AddressSet, CreateFunctionCmd, DisassembleCommand
    from ghidra.app.cmd.disassemble import DisassembleCommand
    from ghidra.app.cmd.function import CreateFunctionCmd
    from ghidra.program.model.address import AddressSet

    project = wwhd_env.open_project(args.project)
    try:
        with pyghidra.program_context(project, wwhd_env.PROGRAM) as program:
            fm = program.getFunctionManager()
            monitor = pyghidra.task_monitor()
            with pyghidra.transaction(program, "split non-contiguous functions"):
                text = program.getMemory().getBlock(".text")
                for n in range(1, MAX_PASSES + 1):
                    split = [f for f in fm.getFunctions(True)
                             if not f.isExternal() and f.getBody().getNumAddressRanges() > 1]
                    created = 0
                    for fn in split:
                        entry = fn.getEntryPoint()
                        body = fn.getBody()
                        primary = body.getRangeContaining(entry)
                        detached = [r.getMinAddress() for r in body.getAddressRanges() if r != primary]
                        if primary.getMinAddress() < entry:
                            detached.append(primary.getMinAddress())
                        fn.setBody(AddressSet(entry, primary.getMaxAddress()))
                        for start in detached:
                            if fm.getFunctionAt(start) is None and CreateFunctionCmd(start).applyTo(program, monitor):
                                created += 1
                    repaired, removed = repair_undisassembled(program, fm, text, monitor)
                    merged, early = fix_early_bodies(program, fm, monitor)
                    cut = cut_address_taken(program, fm, address_taken, monitor)
                    filled = fill_gaps(program, fm, text, address_taken, monitor)
                    print(f"pass {n}: repaired {repaired} / removed {removed} non-code entries, "
                          f"split {len(split)} non-contiguous ({created} new), "
                          f"loop-rotation merges {merged}, early-body splits {early}, "
                          f"cut {cut} at address-taken code, filled {filled} in gaps")
                    if not (repaired or removed or split or merged or early or cut or filled):
                        break
                remaining = sum(1 for f in fm.getFunctions(True)
                                if not f.isExternal() and f.getBody().getNumAddressRanges() > 1)
                print(f"non-contiguous functions remaining: {remaining}")
            program.save("split non-contiguous functions", monitor)
    finally:
        project.close()


if __name__ == "__main__":
    sys.exit(main())
