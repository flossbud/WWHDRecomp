"""The frame log (WWHD_FRAME_LOG, src/overrides/pacing.cpp) summarised: where a 60 fps frame's time goes.

Usage:
    python3 tools/sixty/frames.py LOG [--window N] [--from SWAP] [--to SWAP]

LOG has a line per frame of a real-time run: the swap, the tick (w whole, h half), when the frame's
work began, the work until fw_waitForVsync (ms), of which GX2DrawDone waited for the GPU, the
scheduler thread's CPU time and idle time during the work, the GPU thread's CPU time for the
frame's swap, the vsyncs that came during the work (1 or more: the frame missed its vsync), the
wait, and the half tick journal's stores seen and saved (thousands). For every --window frames
(default 600, 10 s at 60 fps) this prints the frame rate, and for whole and half frames apart the
median and 90th percentile of the work, the median GX2DrawDone wait, CPU and GPU time, and how
many missed their vsync; the half frames' journal counts too. Timings only: no game data.
"""
import argparse
import statistics


def pct(values, q):
    if not values:
        return float("nan")
    v = sorted(values)
    return v[min(len(v) - 1, int(q * len(v)))]


def row(frames, label):
    if not frames:
        return f"{label} -"
    work = [f["work"] for f in frames]
    late = sum(1 for f in frames if f["vsyncs"] > 0)
    out = (f"{label} work {statistics.median(work):5.1f}/{pct(work, 0.9):5.1f} ms, drawdone {statistics.median(f['drawdone'] for f in frames):4.1f}, "
           f"cpu {statistics.median(f['cpu'] for f in frames):5.1f}, idle {statistics.median(f['idle'] for f in frames):4.1f}, "
           f"gpu {statistics.median(f['gpu'] for f in frames):5.1f}, wait {statistics.median(f['wait'] for f in frames):4.1f}, "
           f"late {100 * late / len(frames):3.0f}%")
    if label.strip() == "half" and "stores" in frames[0]:
        out += f", journal {statistics.median(f['stores'] for f in frames):.0f}k stores, {statistics.median(f['saved'] for f in frames):.0f}k saved"
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--window", type=int, default=600)
    ap.add_argument("--from", dest="first", type=int, default=0)
    ap.add_argument("--to", dest="last", type=int, default=1 << 31)
    args = ap.parse_args()
    frames = []
    for line in open(args.log):
        if line.startswith("#"):
            continue
        p = line.split()
        f = {"swap": int(p[0]), "half": p[1] == "h", "began": float(p[2]), "work": float(p[3]), "drawdone": float(p[4]),
             "cpu": float(p[5]), "idle": float(p[6]), "gpu": float(p[7]), "vsyncs": int(p[8]), "wait": float(p[9])}
        if len(p) > 11:
            f["stores"], f["saved"] = int(p[10]), int(p[11])
        if args.first <= f["swap"] <= args.last:
            frames.append(f)
    print(f"{len(frames)} frames")
    for i in range(0, len(frames), args.window):
        w = frames[i:i + args.window]
        span = (w[-1]["began"] + w[-1]["work"] + w[-1]["wait"] - w[0]["began"]) / 1000
        print(f"swaps {w[0]['swap']}-{w[-1]['swap']}: {len(w) / span:5.1f} fps over {span:4.1f} s")
        print("  " + row([f for f in w if not f["half"]], "whole"))
        print("  " + row([f for f in w if f["half"]], "half "))


if __name__ == "__main__":
    main()
