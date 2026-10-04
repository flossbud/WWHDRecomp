#!/usr/bin/env python3
"""refpoke.py PIDFILE STEP...: write guest memory of a running emulator from outside, at moments chosen
from the game's own state. For the unmodified game under the reference Cemu, which has none of our
test aids (WWHD_DEBUG_STAGE, WWHD_DEBUG_POKE: overrides of recompiled code): the same warp and the same
pokes, so that an experiment made with the aids can be repeated on the original (session qa, B5: is what
follows Puppet Ganon's defeat the game's own behaviour?). Run on the worker beside tools/reference/run.sh
(REF_PIDFILE=PIDFILE); it reads and writes /proc/PID/mem (same user; the process is stopped for each
write), finds the guest's memory by the game's code at a known address, and times itself by the game's
frame counter (m_Do_main's, 0x1048D0A8) and current stage (g_dComIfG_gameInfo +0x5134).

STEP is "WHEN:ACTION[;ACTION...]", taken in order:
  WHEN    stage=NAME+N   N frames after the current stage's name first reads NAME
          +N             N frames after the step before
  ACTION  stage NAME,point,room,layer   the next stage, as an exit sets it (gameInfo +0x5140; what
                                        WWHD_DEBUG_STAGE writes)
          p PROC OFFSET SIZE VALUE      a process's field (hex offset and value; size 1, 2 or 4), the
                                        process found by its name number in the heap (WWHD_DEBUG_POKE)
          w ADDR SIZE VALUE             a guest address (hex)
Prints each step as it fires. Exits when the emulator does, or after the last step with --quit.
The moments are a few frames coarse (it polls): for experiments whose outcome doesn't hang on a frame."""
import os
import signal
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
GAME_INFO = 0x1046F0B0
FRAME_COUNTER = 0x1048D0A8
KNOWN_CODE = (0x025DE58C, 0x025F172C, 0x0274C264, 0x025D475C)   # function entries: their first words, as in the RPX


def known_word():
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    from rpx_info import Rpx
    rpx = Rpx(os.path.join(ROOT, "orig/0005000010143500_v0/code/cking.rpx"))
    for i in range(len(rpx.sh)):
        if rpx.name(i) == ".text":
            sh = rpx.sh[i]
            base = sh["addr"] if isinstance(sh, dict) else getattr(sh, "addr", None) or sh[3]
            return [rpx.data(i)[a - base:a - base + 4] for a in KNOWN_CODE]
    sys.exit("refpoke: no .text in the RPX")


class Guest:
    def __init__(self, pid):
        self.pid = pid
        self.mem = open(f"/proc/{pid}/mem", "r+b", buffering=0)
        self.base = None

    def find_base(self, words):
        """The host address of guest address 0: a mapping holding the game's code at KNOWN_CODE (the
        emulator maps each guest region at its own address above one base; the code's may begin at
        any megabyte up to the RPX's 0x02000000)."""
        for line in open(f"/proc/{self.pid}/maps"):
            span, perms = line.split()[:2]
            lo, hi = (int(x, 16) for x in span.split("-"))
            if "r" not in perms or hi - lo < 0x100000:
                continue
            for guest_start in range(0, 0x02100000, 0x00100000):
                base = lo - guest_start
                if base < 0 or not all(lo <= base + a < hi - 4 for a in KNOWN_CODE):
                    continue
                try:
                    same = 0
                    for a, word in zip(KNOWN_CODE, words):
                        self.mem.seek(base + a)
                        same += self.mem.read(4) == word
                    if same >= len(KNOWN_CODE) - 1:
                        self.base = base
                        return True
                except OSError:
                    pass
        return False

    def read(self, ea, n):
        self.mem.seek(self.base + ea)
        return self.mem.read(n)

    def u32(self, ea):
        return struct.unpack(">I", self.read(ea, 4))[0]

    def write(self, ea, data):
        os.kill(self.pid, signal.SIGSTOP)
        try:
            self.mem.seek(self.base + ea)
            self.mem.write(data)
        finally:
            os.kill(self.pid, signal.SIGCONT)

    def stage(self):
        return self.read(GAME_INFO + 0x5134, 8).split(b"\0")[0].decode("ascii", "replace")

    def find_process(self, name):
        """A live process by its name number: its header has the name at +8 and +0xE and a profile in .data."""
        for lo, hi in ((0x47000000, 0x48800000), (0x40000000, 0x50000000), (0x10000000, 0x40000000)):
            try:
                blob = self.read(lo, hi - lo)
            except OSError:
                continue
            key = struct.pack(">H", name)
            at = blob.find(key)
            while at >= 0:
                p = at - 8
                if p >= 0 and p % 4 == 0 and blob[p + 0xE:p + 0x10] == key and blob[p:p + 4] != b"\0\0\0\0":
                    profile = struct.unpack(">I", blob[p + 0x10:p + 0x14])[0]
                    if 0x10000000 <= profile < 0x10500000:
                        return lo + p
                at = blob.find(key, at + 2)
        return None


def main():
    args = [a for a in sys.argv[1:] if a != "--quit"]
    quit_after = "--quit" in sys.argv
    if len(args) < 2:
        sys.exit(__doc__)
    pidfile, steps = args[0], args[1:]
    for _ in range(600):
        if os.path.exists(pidfile):
            break
        time.sleep(0.1)
    pid = int(open(pidfile).read())
    g = Guest(pid)
    word = known_word()
    while not g.find_base(word):
        if not os.path.exists(f"/proc/{pid}"):
            sys.exit("refpoke: the emulator is gone before its memory was found")
        time.sleep(0.5)
    print(f"refpoke: pid {pid}, guest memory at {g.base:#x}", flush=True)
    last_fire = None
    for step in steps:
        when, _, actions = step.partition(":")
        if when.startswith("stage="):
            name, _, n = when[6:].partition("+")
            while g.stage() != name:
                if not os.path.exists(f"/proc/{pid}"):
                    sys.exit(f"refpoke: the emulator ended before stage {name}")
                time.sleep(0.02)
            start = g.u32(FRAME_COUNTER)
        else:
            n, start = when.lstrip("+"), last_fire
        while g.u32(FRAME_COUNTER) - start < int(n or 0):
            if not os.path.exists(f"/proc/{pid}"):
                sys.exit(f"refpoke: the emulator ended before step {step}")
            time.sleep(0.005)
        last_fire = g.u32(FRAME_COUNTER)
        for action in actions.split(";"):
            f = action.split()
            if f[0] == "stage":
                name, point, room, layer = f[1].split(",")
                rec = name.encode().ljust(8, b"\0") + struct.pack(">hbbbB", int(point), int(room), int(layer), 1, 0)
                g.write(GAME_INFO + 0x5140, rec)
            elif f[0] in ("p", "w"):
                if f[0] == "p":
                    proc = g.find_process(int(f[1]))
                    if proc is None:
                        print(f"refpoke: frame {last_fire}: no process {f[1]}: {action} NOT done", flush=True)
                        continue
                    ea, size, value = proc + int(f[2], 16), int(f[3]), int(f[4], 16)
                else:
                    ea, size, value = int(f[1], 16), int(f[2]), int(f[3], 16)
                g.write(ea, value.to_bytes(size, "big"))
                action += f" (at {ea:08x})"
            else:
                sys.exit(f"refpoke: unknown action {action}")
            print(f"refpoke: frame counter {last_fire}, stage {g.stage()}: {action}", flush=True)
    if not quit_after:
        while os.path.exists(f"/proc/{pid}"):
            time.sleep(1)


if __name__ == "__main__":
    main()
