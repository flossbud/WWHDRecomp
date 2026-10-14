#!/usr/bin/env python3
"""flight2route.py FLIGHT.bin [--from FRAME] [--env 'NAME=VALUE' ...] > ROUTE.txt

A route (tools/reference/routes/: FRAME BUTTONS [FRAMES_HELD] in game frames) from the flight recorder's controller
track (overrides/sixty.cpp, WWHD_FLIGHT; WWHD_FLIGHT_EVERY writes it periodically on a device with no F9): what the
owner played, played back. Each whole tick's record of process 0xFFFF holds the swap, the buttons (os/input.h's
bits), the four sticks and the host clock; runs of the same input become one line, the sticks as LS=x,y and RS=x,y.
--from drops the frames before FRAME (menus played by hand that the route's own lines replace); --env adds #env lines
(the run's WWHD_DEBUG_STAGE, WWHD_DEBUG_BOOT...). The recording is the owner's input only: no game data.
"""
import argparse
import struct
import sys

BUTTONS = ["A", "B", "X", "Y", "L", "R", "ZL", "ZR", "PLUS", "MINUS", "HOME", "UP", "DOWN", "LEFT", "RIGHT",
           "LCLICK", "RCLICK"]


def pads(path):
    """{game frame: (buttons, lx, ly, rx, ry)} from the whole ticks' controller records"""
    out = {}
    data = open(path, "rb").read()
    at = 0
    while at + 16 <= len(data):
        tick2, proc_id, _proc, size = struct.unpack_from("<4I", data, at)
        at += 16
        if proc_id == 0xFFFF and size == 28 and tick2 % 2 == 0:
            _swap, buttons, lx, ly, rx, ry, _clock = struct.unpack_from("<2I4fI", data, at)
            out[tick2 // 2] = (buttons, round(lx, 2), round(ly, 2), round(rx, 2), round(ry, 2))
        at += size
    return out


def names(pad):
    buttons, lx, ly, rx, ry = pad
    n = [b for i, b in enumerate(BUTTONS) if buttons & (1 << i)]
    if lx or ly:
        n.append(f"LS={lx:g},{ly:g}")
    if rx or ry:
        n.append(f"RS={rx:g},{ry:g}")
    return "+".join(n)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("flight")
    ap.add_argument("--from", dest="start", type=int, default=0)
    ap.add_argument("--env", action="append", default=[])
    a = ap.parse_args()
    p = pads(a.flight)
    if not p:
        sys.exit("flight2route: no controller records in " + a.flight)
    frames = sorted(f for f in p if f >= a.start)
    print(f"# Route: recorded with the flight recorder ({a.flight.split('/')[-1]}), game frames {frames[0]}-{frames[-1]}")
    for e in a.env:
        print(f"#env {e}")
    print("# FRAME BUTTONS [FRAMES_HELD]; game frames.")
    run_start, run = None, None
    for f in range(frames[0], frames[-1] + 2):
        cur = names(p[f]) if f in p else ""
        if cur != run:
            if run:
                print(f"{run_start} {run} {f - run_start}")
            run_start, run = f, cur
    if run:
        print(f"{run_start} {run} {frames[-1] + 1 - run_start}")


if __name__ == "__main__":
    main()
