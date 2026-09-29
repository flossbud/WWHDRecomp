# /// script
# requires-python = ">=3.10"
# dependencies = ["zstandard", "numpy"]
# ///
"""Read HLE call traces written by the patched reference Cemu (CEMU_HLE_TRACE=file.zst).

Usage:
    uv run tools/reference/hle_trace.py summary TRACE            # calls per function, frames
    uv run tools/reference/hle_trace.py dump TRACE [--frames A-B] [--grep NAME]
    uv run tools/reference/hle_trace.py diff TRACE_A TRACE_B     # first diverging call

Format (see tools/reference/cemu-patches): an 8-byte magic "HLETRC01", then tagged records
    'N' u16 index, u16 length, name
    'C' u32 frame, u16 index, u8 core, u8 pad, u32 thread, u32 lr, u32 r3..r10, f32 f1..f8
A trace from a killed process ends mid-record; the reader stops at the last complete one.
Everything streams in 64 MB chunks (call records parsed in bulk with numpy), so memory use is
bounded no matter how big the trace is. Never load a whole trace into memory: that is how
a 272 MB trace took down the editing machine container on 2026-09-28.
"""
import argparse
import collections
import re
import sys

import numpy as np
import zstandard

CALL = np.dtype([("frame", "<u4"), ("index", "<u2"), ("core", "u1"), ("pad", "u1"),
                 ("thread", "<u4"), ("lr", "<u4"), ("gpr", "<u4", 8), ("fpr", "<f4", 8)])
CALL_REC = 1 + CALL.itemsize  # tag + payload
TAG_C, TAG_N = ord("C"), ord("N")


CHUNK = 64 << 20  # decompressed bytes per step: memory stays bounded whatever the trace size


def blocks(path):
    """Stream a trace: yield (names, calls) where calls is a structured CALL array for the next
    chunk of call records and names is the index -> name table seen so far (grows as it goes)."""
    names = {}
    with open(path, "rb") as f:
        reader = zstandard.ZstdDecompressor().stream_reader(f, read_across_frames=True)
        buf = b""
        eof = False
        first = True
        while True:
            if not eof and len(buf) < CHUNK:
                try:
                    chunk = reader.read(CHUNK)
                except zstandard.ZstdError:
                    chunk = b""  # truncated final block of a killed run
                eof = not chunk
                buf += chunk
            if first:
                if buf[:8] != b"HLETRC01":
                    sys.exit(f"{path}: not an HLE trace")
                buf, first = buf[8:], False
            raw = np.frombuffer(buf, dtype=np.uint8)
            pos, end, parts = 0, len(buf), []
            while pos < end:
                tag = raw[pos]
                if tag == TAG_N:
                    if pos + 5 > end:
                        break
                    length = int.from_bytes(buf[pos + 3:pos + 5], "little")
                    if pos + 5 + length > end:
                        break
                    names[int.from_bytes(buf[pos + 1:pos + 3], "little")] = buf[pos + 5:pos + 5 + length].decode()
                    pos += 5 + length
                elif tag == TAG_C:
                    count = (end - pos) // CALL_REC
                    if count == 0:
                        break
                    tags = raw[pos:pos + count * CALL_REC:CALL_REC]
                    stop = np.flatnonzero(tags != TAG_C)
                    run = int(stop[0]) if stop.size else count
                    block = raw[pos:pos + run * CALL_REC].reshape(run, CALL_REC)[:, 1:]
                    parts.append(np.ascontiguousarray(block).view(CALL).reshape(run))
                    pos += run * CALL_REC
                else:
                    sys.exit(f"{path}: corrupt record tag {tag:#x}")
            buf = buf[pos:]
            if parts:
                yield names, np.concatenate(parts)
            if eof and (pos == 0 or not buf):
                return


def calls(path):
    """Yield (names, single call record) one at a time (for dump and diff reporting)."""
    for names, block in blocks(path):
        for c in block:
            yield names, c


HOST_ADDRESS = re.compile(r"^(PPCCallback)[0-9a-f]+$")


def fmt(names, c, core=True):
    args = " ".join(f"{g:08x}" for g in c["gpr"])
    floats = " ".join(f"{x:g}" for x in c["fpr"] if x)
    # Cemu names callback stubs after a host pointer, which ASLR changes every run
    name = HOST_ADDRESS.sub(r"\1", names.get(int(c["index"]), f"#{c['index']}"))
    return (f"f{c['frame']:6d} {'c%d ' % c['core'] if core else ''}t{c['thread']:08x} lr{c['lr']:08x} {name}({args})"
            + (f" [{floats}]" if floats else ""))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("summary"); s.add_argument("trace")
    d = sub.add_parser("dump"); d.add_argument("trace"); d.add_argument("--frames"); d.add_argument("--grep")
    x = sub.add_parser("diff"); x.add_argument("a"); x.add_argument("b")
    x.add_argument("--ignore-core", action="store_true",
                   help="compare without the core index (which core served a thread)")
    args = ap.parse_args()

    if args.cmd == "summary":
        counts, total, frames, names = collections.Counter(), 0, 0, {}
        for names, block in blocks(args.trace):
            idx, n = np.unique(block["index"], return_counts=True)
            counts.update(dict(zip(idx.tolist(), n.tolist())))
            total += len(block)
            frames = max(frames, int(block["frame"].max()))
        print(f"{total} calls, {len(counts)} functions, {frames} frames")
        for i, n in counts.most_common():
            print(f"{n:10d}  {names.get(i, f'#{i}')}")
    elif args.cmd == "dump":
        lo, hi = 0, float("inf")
        if args.frames:
            a, _, b = args.frames.partition("-")
            lo, hi = int(a), int(b or a)
        for names, c in calls(args.trace):
            if c["frame"] > hi:
                break
            if c["frame"] >= lo and (not args.grep or args.grep in names.get(int(c["index"]), "")):
                print(fmt(names, c))
    elif args.cmd == "diff":
        sys.exit(diff(args.a, args.b, args.ignore_core))


def diff(path_a, path_b, ignore_core):
    """Vectorized comparison of two traces. Function indices are mapped to normalized names (so
    runs with different HLE index tables or host-pointer stub names still compare), then whole
    numpy blocks are compared at once; only the first mismatch is rendered."""
    def canon(names):
        return {i: HOST_ADDRESS.sub(r"\\1", n) for i, n in names.items()}

    name_ids = {}

    def keyed(names, block):
        c = canon(names)
        lut = np.zeros(0x10000, dtype=np.uint32)
        for i, n in c.items():
            lut[i] = name_ids.setdefault(n, len(name_ids) + 1)
        k = block.copy()
        k["index"] = lut[block["index"]].astype(np.uint16)
        k["pad"] = 0
        if ignore_core:
            k["core"] = 0
        return k

    ga, gb = blocks(path_a), blocks(path_b)
    buf_a = buf_b = np.zeros(0, CALL)
    names_a = names_b = {}
    offset = 0
    done_a = done_b = False
    while True:
        while len(buf_a) < 1 << 20 and not done_a:
            try:
                names_a, blk = next(ga); buf_a = np.concatenate([buf_a, keyed(names_a, blk)])
            except StopIteration:
                done_a = True
        while len(buf_b) < 1 << 20 and not done_b:
            try:
                names_b, blk = next(gb); buf_b = np.concatenate([buf_b, keyed(names_b, blk)])
            except StopIteration:
                done_b = True
        n = min(len(buf_a), len(buf_b))
        if n == 0:
            break
        # compare raw bytes: float args are often NaN, and NaN != NaN field-wise
        bad = np.flatnonzero((buf_a[:n].view(np.uint8).reshape(n, -1)
                              != buf_b[:n].view(np.uint8).reshape(n, -1)).any(axis=1))
        if bad.size:
            i = int(bad[0])
            inv = {v: k for k, v in name_ids.items()}
            def show(c):
                nm = inv.get(int(c["index"]), "?")
                return fmt({int(c["index"]): nm}, c, not ignore_core)
            print(f"first difference at call {offset + i}:")
            for j in range(max(0, i - 3), i):
                print(f"   A {show(buf_a[j])}\n   B {show(buf_b[j])}")
            print(f">> A {show(buf_a[i])}\n>> B {show(buf_b[i])}")
            return 1
        offset += n
        buf_a, buf_b = buf_a[n:], buf_b[n:]
    if len(buf_a) or len(buf_b):
        print(f"identical for {offset} calls, then {'A' if len(buf_a) else 'B'} continues")
        return 1
    print(f"identical: {offset} calls")
    return 0


if __name__ == "__main__":
    main()
