# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy"]
# ///
"""Compare the GPU-side surfaces of the reference and the recomp at one frame (milestone G2).

Usage:
    uv run tools/reference/compare_dumps.py FRAME REF_DIR OURS_DIR

REF_DIR holds the reference's texture dump (cemu-patches/0012: CEMU_TEX_DUMP_FRAME=FRAME,
CEMU_TEX_DUMP_DIR), tex<FRAME>_<addr>_<mipaddr>_<fmt>[d]_<w>x<h>_m<mip>_<gpu|ram>_w<counter>.ppm.
OURS_DIR holds the renderer's (WWHD_RENDER_DUMP=FRAME), dump<FRAME>_<addr>_<fmt>[d]_<w>x<h>.ppm.
Both are taken at the FRAME-th swap with the same conversion to 8-bit RGB.

Pairs each GPU-written reference texture with our surface at the same address and GX2 format
(level 1 of a mip chain with our surface at its mip address; deeper levels are skipped), crops
ours to the reference's size, and prints them in the reference's write order: the first real
difference in that order is where to look. Depth and single-channel float surfaces are
stretched to their own value range in both dumps, so their numbers only say "same" or "not".
"""
import glob
import os
import re
import sys

import numpy as np

REF = re.compile(r"tex\d+_([0-9a-f]{8})_([0-9a-f]{8})_([0-9a-f]+d?)_(\d+)x(\d+)_m(\d+)_(\w+)_w(\d+)\.ppm")
OURS = re.compile(r"dump\d+_([0-9a-f]{8})_([0-9a-f]+d?)_(\d+)x(\d+)\.ppm")
STRETCHED = {"806", "80e", "80ed", "5d", "11d", "81cd", "811d", "5", "80d"}   # R32F, depth formats


def read_ppm(path):
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
    return np.frombuffer(data[pos:pos + w * h * 3], np.uint8).reshape(h, w, 3).astype(np.float32)


def main():
    frame, ref_dir, ours_dir = f"{int(sys.argv[1]):06}", sys.argv[2], sys.argv[3]
    ours = {}
    for p in glob.glob(f"{ours_dir}/dump{frame}_*.ppm"):
        m = OURS.match(os.path.basename(p))
        if m:
            ours[(m[1], m[2])] = p
    rows = []
    for p in glob.glob(f"{ref_dir}/tex{frame}_*.ppm"):
        m = REF.match(os.path.basename(p))
        if not m or m[7] != "gpu":
            continue
        addr, mipaddr, fmt, w, h, level, counter = m[1], m[2], m[3], int(m[4]), int(m[5]), int(m[6]), int(m[8])
        if level > 1 or (level == 1 and mipaddr == "00000000"):
            continue
        key = (addr if level == 0 else mipaddr, fmt)
        name = f"{addr} {mipaddr} {fmt:>5} {w}x{h} m{level}"
        note = " (stretched)" if fmt in STRETCHED else ""
        if key not in ours:
            rows.append((counter, name, "no surface here", "", "", ""))
            continue
        a, b = read_ppm(p), read_ppm(ours[key])
        hh, ww = min(h, b.shape[0]), min(w, b.shape[1])
        a, b = a[:hh, :ww], b[:hh, :ww]
        d = np.abs(a - b)
        rows.append((counter, name, f"{d.mean():7.2f}", f"{(b - a).mean():+7.2f}", f"{(d.max(axis=2) > 16).mean() * 100:5.1f}%", note))
    rows.sort()
    print(f"{'write':>8}  {'reference texture':42} {'|diff|':>7} {'ours-ref':>8} {'>16':>6}")
    for counter, name, diff, bias, share, note in rows:
        print(f"{counter:>8}  {name:42} {diff:>7} {bias:>8} {share:>6}{note}")


if __name__ == "__main__":
    main()
