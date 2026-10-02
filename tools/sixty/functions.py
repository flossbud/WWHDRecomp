"""The function a guest address is in (config/US_v0/functions.csv), for the 60 fps tools' reports."""
import bisect
import csv
import os

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')


class Functions:
    def __init__(self, path=os.path.join(ROOT, 'config/US_v0/functions.csv')):
        rows = sorted((int(r['address'], 16), int(r['end'], 16)) for r in csv.DictReader(open(path)))
        self.starts = [a for a, _ in rows]
        self.ends = [e for _, e in rows]

    def entry(self, pc):
        """The entry of the function containing pc, or None."""
        i = bisect.bisect_right(self.starts, pc) - 1
        return self.starts[i] if i >= 0 and pc < self.ends[i] else None

    def function(self, pc):
        """f_XXXXXXXX, the function containing a guest address (0: hand-written code)."""
        if pc == 0:
            return 'override'
        e = self.entry(pc)
        return f'f_{e:08X}' if e is not None else f'{pc:08x}'

    def name(self, pc):
        """f_XXXXXXXX+off for a guest address (0: hand-written code)."""
        if pc == 0:
            return 'override'
        e = self.entry(pc)
        return f'f_{e:08X}+{pc - e:#x}' if e is not None else f'{pc:08x}'
