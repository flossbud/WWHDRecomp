# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy"]
# ///
"""Compare the TV frames of two runs (design D16.3; milestone G2).

Usage:
    uv run tools/reference/compare_frames.py REF_DIR TEST_DIR [--threshold DB]

Both directories hold f<frame>.tv.ppm captures (CEMU_SHOT_FRAMES/CEMU_SHOT_DIR): the reference's
from its patched Cemu, ours from wwhd-null with WWHD_RENDER=vk. For each frame both have: the PSNR
of TEST against REF (inf when identical), the largest channel difference, and the share of pixels
that differ by more than 16 in any channel. With --threshold, exit 1 unless every frame reaches it.
"""
import argparse
import pathlib
import sys

import numpy as np


def read_ppm(path):
    data = path.read_bytes()
    parts, pos = [], 0
    while len(parts) < 4:                               # magic, width, height, maxval
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            pos = data.index(b"\n", pos) + 1
            continue
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        parts.append(data[pos:end])
        pos = end
    if parts[0] != b"P6" or parts[3] != b"255":
        sys.exit(f"{path}: not an 8-bit P6 image")
    w, h = int(parts[1]), int(parts[2])
    return np.frombuffer(data, np.uint8, w * h * 3, pos + 1).reshape(h, w, 3)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ref")
    ap.add_argument("test")
    ap.add_argument("--threshold", type=float, help="minimum PSNR (dB) every frame must reach")
    args = ap.parse_args()
    ref, test = pathlib.Path(args.ref), pathlib.Path(args.test)
    frames = sorted(p.name for p in ref.glob("f*.tv.ppm") if (test / p.name).exists())
    if not frames:
        sys.exit("no frames in common")
    worst = float("inf")
    print(f"{'frame':>14}  {'PSNR dB':>8}  {'max diff':>8}  {'pixels >16':>10}")
    for name in frames:
        a, b = read_ppm(ref / name), read_ppm(test / name)
        if a.shape != b.shape:
            print(f"{name:>14}  size {a.shape[1]}x{a.shape[0]} vs {b.shape[1]}x{b.shape[0]}")
            worst = -1
            continue
        d = np.abs(a.astype(np.int16) - b.astype(np.int16))
        mse = float(np.mean(d.astype(np.float64) ** 2))
        psnr = float("inf") if mse == 0 else 10 * np.log10(255 ** 2 / mse)
        share = float(np.mean(d.max(axis=2) > 16)) * 100
        worst = min(worst, psnr)
        print(f"{name:>14}  {psnr:8.2f}  {int(d.max()):8d}  {share:9.2f}%")
    print(f"{len(frames)} frames; worst PSNR {worst:.2f} dB")
    if args.threshold is not None and worst < args.threshold:
        sys.exit(1)


if __name__ == "__main__":
    main()
