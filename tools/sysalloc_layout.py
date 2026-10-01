"""Make src/os/common/sysalloc_layout.h: where each of Cemu's SysAllocators lives in guest memory in the
reference (design D18: the guest OS objects' own home).

Usage:
    python3 tools/sysalloc_layout.py LOG > src/os/common/sysalloc_layout.h

LOG is what CEMU_SYSALLOC_LOG=path writes (cemu-patches/0016) in a run whose layout is the
reference's: the reference Cemu itself, or a wwhd-null that matches its traces. One line per slot,
in the order they were laid out: declaring file, line, size, alignment, offset in the Cemu area
(0x0E000000); then "end" with the first free offset after them.

A slot is known by the file that declares it, its size and alignment, and how many alike that file
declared before it (the ordinal): lines move when a fork is edited, the order of a file's
declarations doesn't. For a header the line is part of the key too (headers aren't forked); slots
that are members of a class declared in a header, one per object, share it and differ only by
ordinal, and as they are the same size, which object takes which never moves another slot.
"""
import os
import sys

HEADER = """// The reference's guest-memory layout of Cemu's SysAllocators (design D18): each slot's offset in the
// Cemu area (0x0E000000), keyed by the file that declares it (with the line, for a header), its size
// and alignment, and how many alike that file declared before it. src/os/common/SysAllocator.cpp lays
// slots out from it, so guest memory is the reference's whatever the link order. Made by
// tools/sysalloc_layout.py from CEMU_SYSALLOC_LOG; make it again when Cemu or a fork's slots change.
#pragma once

struct SysAllocatorSlot
{
	const char* file;
	uint32 line;       // 0 unless a header
	uint32 size, alignment, ordinal, offset;
};
"""


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    slots, seen, end = [], {}, None
    for n, line in enumerate(open(sys.argv[1]), 1):
        path, decl_line, size, align, offset = line.rstrip("\n").split("\t")
        if path == "end":
            end = int(offset, 16)
            continue
        name = os.path.basename(path)
        header = name.endswith((".h", ".hpp"))
        key = (name, int(decl_line) if header else 0, int(size), int(align))
        ordinal = seen.get(key, 0)
        seen[key] = ordinal + 1
        slots.append((*key, ordinal, int(offset, 16)))
    if end is None:
        sys.exit(f"{sys.argv[1]}: no end line (an old log?)")
    out = [HEADER, f"static constexpr SysAllocatorSlot kSysAllocatorLayout[] = {{"]
    for name, line, size, align, ordinal, offset in slots:
        out.append(f'\t{{"{name}", {line}, {size:#x}, {align}, {ordinal}, {offset:#010x}}},')
    out.append("};")
    out.append(f"static constexpr uint32 kSysAllocatorLayoutEnd = {end:#010x};   // the first free offset after them")
    print("\n".join(out))
    print(f"{len(slots)} slots, {len({s[0] for s in slots})} files, end at {end:#x}", file=sys.stderr)


if __name__ == "__main__":
    main()
