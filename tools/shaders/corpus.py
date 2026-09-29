# /// script
# requires-python = ">=3.10"
# dependencies = ["oead"]
# ///
"""G1 (docs/recompiler-design.md D14): collect every GX2 shader program in the game's files.

Usage:
    uv run tools/shaders/corpus.py CONTENT_DIR OUT_DIR [--seen VARIANTS_CSV]

Walks CONTENT_DIR (the title's content/ folder), opening containers recursively: Yaz0 (.szs),
SARC (.sarc, .pack and what Yaz0 holds), SHARCFB (agl's shader archives, "BAHS") and GFD
(.gsh, "Gfx2"). Every compiled program becomes one entry, keyed by the FNV-1a hash of its
microcode as it sits in guest memory: the same hash the null GPU's G0 statistics use
(WWHD_GPU_STATS, src/gpu/null_gpu.cpp). OUT_DIR gets, for each distinct program,
programs/<hash>.<vs|ps|gs> (a small header, the GX2 shader struct's registers and the
microcode; see write_program) and index.csv. Game data: OUT_DIR must be under build/ or the
worker's data volume, never in git.

With --seen (a G0 variants.csv), reports which programs seen at runtime are in the corpus.
"""
import argparse
import collections
import csv
import pathlib
import struct
import sys

import oead

STAGES = {0: "vs", 1: "ps", 2: "gs"}


def fnv(data):
    """FNV-1a 64 over bytes (gpustats::Fnv in src/gpu/null_gpu.cpp)."""
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


class Program:
    def __init__(self, stage, regs, mode, code):
        self.stage, self.regs, self.mode, self.code = stage, regs, mode, code
        self.hash = None            # computed once per distinct program (main)
        self.sources = []


# ---- GX2 shader structs (Cemu's Cafe/OS/libs/gx2/GX2_Shader.h) --------------------------------
# field offsets: registers, then size, program pointer, shader mode
LAYOUT = {"vs": (52, 0xD0, 0xD4, 0xD8), "ps": (41, 0xA4, 0xA8, 0xAC), "gs": (19, 0x4C, 0x50, 0x5C)}


def gx2_program(stage, struct_bytes, endian, code_at):
    """A GX2 shader struct plus a function mapping its program pointer to the microcode."""
    nregs, size_off, ptr_off, mode_off = LAYOUT[stage]
    regs = list(struct.unpack_from(f"{endian}{nregs}I", struct_bytes, 0))
    size, ptr, mode = (struct.unpack_from(f"{endian}I", struct_bytes, o)[0] for o in (size_off, ptr_off, mode_off))
    return Program(stage, regs, mode, code_at(ptr, size))


def sharcfb(data, where, out):
    """agl's shader archive (magic "BAHS"). Version 9 is little-endian throughout; the binary
    section holds one GX2 struct per program, whose pointers are offsets into the binary."""
    endian = "<" if struct.unpack_from("<I", data, 12)[0] == 1 else ">"
    u = lambda o: struct.unpack_from(f"{endian}I", data, o)[0]
    version, name_len = u(4), u(20)
    if version != 9:
        print(f"{where}: SHARCFB version {version}, only 9 is known", file=sys.stderr)
    pos = 24 + name_len
    section, count = u(pos), u(pos + 4)
    p = pos + 8
    for i in range(count):
        size, kind, _flags, blob_size = u(p), u(p + 4), u(p + 8), u(p + 12)
        blob = data[p + 16:p + 16 + blob_size]
        stage = STAGES.get(kind)
        if stage is None:
            print(f"{where}: binary {i} has unknown type {kind}", file=sys.stderr)
        else:
            prog = gx2_program(stage, blob, endian, lambda ptr, n: blob[ptr:ptr + n])
            out.append((prog, f"{where}#{i}"))
        p += size
    if p != pos + section:
        print(f"{where}: binary section ends at {p:#x}, header says {pos + section:#x}", file=sys.stderr)


def gfd(data, where, out):
    """GX2 shader file ("Gfx2", big-endian): blocks "BLK{", a shader header block (the GX2 struct)
    followed by its program block (the microcode)."""
    HEADERS = {3: "vs", 6: "ps", 8: "gs"}
    PROGRAMS = {5: "vs", 7: "ps", 9: "gs"}
    p = struct.unpack_from(">I", data, 4)[0]
    pending = {}
    n = 0
    while p + 0x20 <= len(data) and data[p:p + 4] == b"BLK{":
        hsize, _maj, _min, kind, dsize = struct.unpack_from(">IIIII", data, p + 4)
        body = data[p + hsize:p + hsize + dsize]
        if kind in HEADERS:
            pending[HEADERS[kind]] = body
        elif kind in PROGRAMS:
            stage = PROGRAMS[kind]
            prog = gx2_program(stage, pending.pop(stage), ">", lambda ptr, size, body=body: body[:size])
            out.append((prog, f"{where}#{n}"))
            n += 1
        elif kind == 1:
            break
        p += hsize + dsize


def walk(data, where, out, counts):
    """Open containers until shader files turn up."""
    magic = data[:4]
    if magic == b"Yaz0":
        counts["yaz0"] += 1
        walk(bytes(oead.yaz0.decompress(data)), where, out, counts)
    elif magic == b"SARC":
        counts["sarc"] += 1
        for f in oead.Sarc(data).get_files():
            walk(bytes(f.data), f"{where}/{f.name}", out, counts)
    elif magic == b"BAHS":
        counts["sharcfb"] += 1
        sharcfb(data, where, out)
    elif magic == b"Gfx2":
        counts["gsh"] += 1
        gfd(data, where, out)
    else:
        # shader files embedded in other files: BFRES keeps GX2 shader files among its external
        # files (Common/Misc/Misc.bfres holds one), so look inside every other leaf too
        for sig, parse in ((b"Gfx2", gfd), (b"BAHS", sharcfb)):
            at = data.find(sig)
            while at >= 0:
                ok = (sig == b"Gfx2" and data[at + 4:at + 8] == b"\0\0\0\x20") or \
                     (sig == b"BAHS" and data[at + 4:at + 8] == b"\x09\0\0\0")
                if ok:
                    counts[f"embedded {sig.decode()}"] += 1
                    parse(data[at:], f"{where}@{at:#x}", out)
                at = data.find(sig, at + 4)


def write_program(path, prog):
    """programs/<hash>.<stage>: "WWSH", u32 version 1, u32 stage (0 vs, 1 ps, 2 gs), u32 shader
    mode, u32 register count, the registers, u32 microcode size, the microcode (little-endian)."""
    stage = {"vs": 0, "ps": 1, "gs": 2}[prog.stage]
    head = struct.pack(f"<4sIIII{len(prog.regs)}II", b"WWSH", 1, stage, prog.mode, len(prog.regs), *prog.regs,
                       len(prog.code))
    path.write_bytes(head + prog.code)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("content")
    ap.add_argument("out")
    ap.add_argument("--seen", help="a G0 variants.csv (WWHD_GPU_STATS): check the runtime's programs are here")
    ap.add_argument("--runtime", help="a WWHD_GPU_DUMP directory: classify every program a run used")
    args = ap.parse_args()
    root = pathlib.Path(args.content)
    out_dir = pathlib.Path(args.out)
    (out_dir / "programs").mkdir(parents=True, exist_ok=True)

    found, counts = [], collections.Counter()
    files = sorted(p for p in root.rglob("*") if p.is_file())
    for k, path in enumerate(files):
        walk(path.read_bytes(), str(path.relative_to(root)), found, counts)
        if k % 50 == 0:
            print(f"... {k}/{len(files)} files, {len(found)} programs", file=sys.stderr, flush=True)   # heartbeat

    distinct = {}                   # the same archives recur in many stages: hash each program once
    for prog, where in found:
        key = (prog.stage, prog.code, tuple(prog.regs), prog.mode)
        if key not in distinct:
            distinct[key] = prog
        distinct[key].sources.append(where)
    corpus = {}
    for prog in distinct.values():
        prog.hash = fnv(prog.code)
        key = (prog.stage, prog.hash)
        if key in corpus:           # same microcode, other registers: keep one, note the rest
            corpus[key].sources += prog.sources
            counts["same code, other registers"] += 1
        else:
            corpus[key] = prog
    for (stage, h), prog in corpus.items():
        write_program(out_dir / "programs" / f"{h:016x}.{stage}", prog)
    with open(out_dir / "index.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["hash", "stage", "size", "mode", "occurrences", "first_source"])
        for (stage, h), prog in sorted(corpus.items()):
            w.writerow([f"{h:016x}", stage, len(prog.code), prog.mode, len(prog.sources), prog.sources[0]])

    by_stage = collections.Counter(stage for stage, _ in corpus)
    print(f"{len(files)} files; containers: {dict(counts)}; {len(found)} programs, "
          f"{len(corpus)} distinct ({dict(by_stage)})")

    if args.runtime:
        classify_runtime(args.runtime, corpus, out_dir)
    if args.seen:
        seen = {"vs": set(), "ps": set(), "gs": set()}
        with open(args.seen) as f:
            for r in csv.DictReader(f):
                for stage, col in (("vs", "vertex"), ("ps", "pixel"), ("gs", "geometry")):
                    if int(r[col], 16):
                        seen[stage].add(int(r[col], 16))
        for stage in ("vs", "ps", "gs"):
            have = {h for s, h in corpus if s == stage}
            missing = sorted(seen[stage] - have)
            print(f"seen at runtime ({stage}): {len(seen[stage])}, in the corpus: {len(seen[stage] & have)}, "
                  f"missing: {len(missing)}" + (f" e.g. {', '.join(f'{m:016x}' for m in missing[:5])}" if missing else ""))


def classify_runtime(dump, corpus, out_dir):
    """Each program a run used (its bytes, from WWHD_GPU_DUMP): in the corpus as it is, in the
    corpus but shorter at runtime (the size register cuts it; the rest is the file's program),
    or runtime-only (not in any file: the game builds it)."""
    by_head = collections.defaultdict(list)
    for (stage, _h), prog in corpus.items():
        by_head[(stage, prog.code[:64])].append(prog)
    rows, tally = [], collections.Counter()
    for path in sorted(pathlib.Path(dump).iterdir()):
        stage = path.suffix[1:]
        if stage not in ("vs", "ps", "gs"):
            continue
        code = path.read_bytes()
        h = int(path.stem, 16)
        if (stage, h) in corpus:
            kind, other = "file", ""
        else:
            longer = [p for p in by_head[(stage, code[:64])] if p.code.startswith(code)]
            kind, other = ("file, shorter at runtime", f"{longer[0].hash:016x}") if longer else ("runtime only", "")
        tally[(stage, kind)] += 1
        rows.append([path.stem, stage, len(code), kind, other])
    with open(pathlib.Path(out_dir) / "runtime.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["hash", "stage", "size", "found", "file_program"])
        w.writerows(rows)
    for (stage, kind), n in sorted(tally.items()):
        print(f"runtime {stage}: {n} {kind}")


if __name__ == "__main__":
    main()
