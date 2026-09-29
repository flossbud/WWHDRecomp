# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy"]
# ///
"""Compare the GPU-side surfaces of the reference and the recomp at one frame (milestone G2).

Usage:
    uv run tools/reference/compare_dumps.py FRAME REF_DIR OURS_DIR

REF_DIR holds the reference's texture dump (cemu-patches/0012: CEMU_TEX_DUMP_FRAME=FRAME,
CEMU_TEX_DUMP_DIR), tex<FRAME>_<addr>_<mipaddr>_<fmt>[d]_<w>x<h>_m<mip>_s<slice>_<gpu|ram>_w<counter>.
OURS_DIR holds the renderer's (WWHD_RENDER_DUMP=FRAME), dump<FRAME>_<addr>_<fmt>[d]_<w>x<h>_s<layer>.
Both are taken at the FRAME-th swap with the same conversion: colour as 8-bit PPM, depth and
single-channel float as 16-bit PGM (values 0..1).

Pairs each GPU-written reference texture (each array slice) with our surface at the same
address, GX2 format and slice (level 1 of a mip chain with our surface at its mip address; deeper
levels are skipped), crops ours to the reference's size, and prints them in the reference's write
order: the first real difference in that order is where to look. Differences are in 8-bit steps
(16-bit grey is scaled down), ">16" is the share of pixels more than 16 steps off.
"""
import glob
import os
import re
import sys

import numpy as np

REF = re.compile(r"tex\d+_([0-9a-f]{8})_([0-9a-f]{8})_([0-9a-f]+d?)_(\d+)x(\d+)_m(\d+)_s(\d+)_(gpu|ram)_w(\d+)\.p[gp]m")
OURS = re.compile(r"dump\d+_([0-9a-f]{8})_([0-9a-f]+d?)_(\d+)x(\d+)_s(\d+)\.p[gp]m")


def read_image(path):
    """P6 (8-bit RGB) or P5 (16-bit grey) as float32 in 8-bit steps, shape (h, w, channels)."""
    data = open(path, "rb").read()
    parts, pos = [], 0
    while len(parts) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        parts.append(data[start:pos])
    w, h = int(parts[1]), int(parts[2])
    pos += 1
    if parts[0] == b"P5":
        grey = np.frombuffer(data[pos:pos + w * h * 2], ">u2").reshape(h, w, 1)
        return grey.astype(np.float32) / 257.0
    return np.frombuffer(data[pos:pos + w * h * 3], np.uint8).reshape(h, w, 3).astype(np.float32)


def main():
    frame, ref_dir, ours_dir = f"{int(sys.argv[1]):06}", sys.argv[2], sys.argv[3]
    ours = {}
    for p in glob.glob(f"{ours_dir}/dump{frame}_*.p[gp]m"):
        m = OURS.match(os.path.basename(p))
        if m:
            ours[(m[1], m[2], int(m[5]))] = p
    rows = []
    for p in glob.glob(f"{ref_dir}/tex{frame}_*.p[gp]m"):
        m = REF.match(os.path.basename(p))
        if not m or m[8] != "gpu":
            continue
        addr, mipaddr, fmt, w, h, level, slice_, counter = m[1], m[2], m[3], int(m[4]), int(m[5]), int(m[6]), int(m[7]), int(m[9])
        if level > 1 or (level == 1 and mipaddr == "00000000"):
            continue
        key = (addr if level == 0 else mipaddr, fmt, slice_)
        name = f"{addr} {mipaddr} {fmt:>5} {w}x{h} m{level} s{slice_}"
        note = ""
        if key not in ours:
            rows.append((counter, name, "no surface here", "", "", ""))
            continue
        a, b = read_image(p), read_image(ours[key])
        hh, ww = min(h, b.shape[0]), min(w, b.shape[1])
        a, b = a[:hh, :ww], b[:hh, :ww]
        d = np.abs(a - b)
        rows.append((counter, name, f"{d.mean():7.2f}", f"{(b - a).mean():+7.2f}", f"{(d.max(axis=2) > 16).mean() * 100:5.1f}%", note))
    rows.sort()
    print(f"{'write':>8}  {'reference texture':45} {'|diff|':>7} {'ours-ref':>8} {'>16':>6}")
    for counter, name, diff, bias, share, note in rows:
        print(f"{counter:>8}  {name:45} {diff:>7} {bias:>8} {share:>6}{note}")


if __name__ == "__main__":
    main()
