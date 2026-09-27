"""Convert a GNU-ld style `name = 0xADDR; /* Real::name */` file into symbols.csv rows.

Used for SuperDude88/TWWHD-Randomizer asm/linker.ld (MIT, targets US v0):
    python3 tools/symbols/from_linker_ld.py linker.ld "twwhd-randomizer@f28d289 asm/linker.ld" \
        >> config/US_v0/symbols.csv
A trailing comment that looks like a C++ qualified name (`dSv_info_c::isSwitch`)
is preferred over the flattened label. Commented-out lines are skipped.
"""
import csv
import re
import sys

LINE = re.compile(r"^\s*([A-Za-z_]\w*)\s*=\s*0x([0-9A-Fa-f]+)\s*;\s*(?:/\*\s*(.*?)\s*\*/)?\s*$")
QUALIFIED = re.compile(r"^[A-Za-z_]\w*(::[A-Za-z_~]\w*)+$")


def main():
    path, evidence = sys.argv[1], sys.argv[2]
    w = csv.writer(sys.stdout, lineterminator="\n")
    for line in open(path):
        m = LINE.match(line)
        if not m:
            continue
        label, addr, comment = m.groups()
        name = comment if comment and QUALIFIED.match(comment) else label
        w.writerow([f"{int(addr, 16):08X}", name, evidence])


if __name__ == "__main__":
    main()
