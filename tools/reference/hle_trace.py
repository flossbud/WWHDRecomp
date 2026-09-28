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


def fmt(names, c):
    args = " ".join(f"{g:08x}" for g in c["gpr"])
    floats = " ".join(f"{x:g}" for x in c["fpr"] if x)
    # Cemu names callback stubs after a host pointer, which ASLR changes every run
    name = HOST_ADDRESS.sub(r"\1", names.get(int(c["index"]), f"#{c['index']}"))
    return (f"f{c['frame']:6d} c{c['core']} t{c['thread']:08x} lr{c['lr']:08x} {name}({args})"
            + (f" [{floats}]" if floats else ""))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("summary"); s.add_argument("trace")
    d = sub.add_parser("dump"); d.add_argument("trace"); d.add_argument("--frames"); d.add_argument("--grep")
    x = sub.add_parser("diff"); x.add_argument("a"); x.add_argument("b")
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
        # walk both traces in step, comparing rendered records (robust to differing index tables)
        n = 0
        history = collections.deque(maxlen=3)
        sa, sb = calls(args.a), calls(args.b)
        for (na, ca), (nb, cb) in zip(sa, sb):
            ra, rb = fmt(na, ca), fmt(nb, cb)
            if ra != rb:
                print(f"first difference at call {n}:")
                for pa, pb in history:
                    print(f"   A {pa}\n   B {pb}")
                print(f">> A {ra}\n>> B {rb}")
                sys.exit(1)
            history.append((ra, rb))
            n += 1
        rest_a, rest_b = next(sa, None), next(sb, None)
        if rest_a or rest_b:
            print(f"identical for {n} calls, then {'A' if rest_a else 'B'} continues")
            sys.exit(1)
        print(f"identical: {n} calls")


if __name__ == "__main__":
    main()
