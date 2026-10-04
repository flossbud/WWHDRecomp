"""Which actors a stage's rooms place: the rooms' actor lists, by WWHD process number (for 60 fps work).

Usage (on the worker; reads the game's files, so its output stays there):
    python3 tools/stage_actors.py STAGE [ROOM...] [--names NAMES.tsv] [--layers] [--pos PROC]

STAGE is a stage name (sea, M_NewD2, kindan...): content/Common/Stage/STAGE_Stage.szs and STAGE_RoomN.szs
(Yaz0-compressed SARC archives in WWHD, RARC on the GameCube; some, e.g. sea rooms 11 and 44, are inside
content/Common/Pack's SARC packs). WWHD embeds the room.dzr / stage.dzs in the archive's .bfres. Each holds a stage.dzs or room.dzr whose chunks place actors: ACTR,
SCOB and TRES (and their layer copies ACT0-ACTb, SCO0-SCOb, TRE0-TREb; with --layers each listed apart),
each entry naming the actor by an 8-byte name. dStage_searchName's table (l_objectName in d_stage.cpp:
{name[8], s16 process, s8 argument, s8 gba}) maps the names to process numbers; WWHD's copy is found in
the RPX's data by its first two names. Prints, per room, the process numbers present with their counts,
profile names (NAMES.tsv, default /wwhd/data/ghidra-out/actor_names.tsv) and stage names. ROOM limits
the rooms (numbers; "stage" for the stage file). --pos PROC prints that process's placements instead
(name, parameters, position, angle y): where to put Link (WWHD_DEBUG_PLACE) or spawn one.
"""
import argparse
import collections
import glob
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from rpx_info import Rpx  # noqa: E402

CONTENT = os.path.join(ROOT, "orig/0005000010143500_v0/content/Common/Stage")
PACKS = os.path.join(ROOT, "orig/0005000010143500_v0/content/Common/Pack")
RPX = os.path.join(ROOT, "orig/0005000010143500_v0/code/cking.rpx")
SIZES = {"ACTR": 0x20, "SCOB": 0x24, "TRES": 0x20}


def yaz0(data):
    """Decompress a Yaz0 stream (the archives' compression)."""
    if data[:4] != b"Yaz0":
        return data
    size = struct.unpack(">I", data[4:8])[0]
    out = bytearray()
    i = 16
    while len(out) < size:
        code = data[i]
        i += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if code & (0x80 >> bit):
                out.append(data[i])
                i += 1
            else:
                b1, b2 = data[i], data[i + 1]
                i += 2
                dist = ((b1 & 0x0F) << 8 | b2) + 1
                n = b1 >> 4
                if n == 0:
                    n = data[i] + 0x12
                    i += 1
                else:
                    n += 2
                for _ in range(n):
                    out.append(out[-dist])
    return bytes(out)


def sarc_bytes(data):
    """{name: bytes} of an in-memory SARC archive (WWHD's room and stage archives are SARC)."""
    hl, _bom, _size, data_off = struct.unpack(">HHII", data[4:16])
    _shl, n, _key = struct.unpack(">HHI", data[hl + 4:hl + 12])
    nodes = [struct.unpack(">IIII", data[hl + 12 + 16 * k:hl + 28 + 16 * k]) for k in range(n)]
    names = hl + 12 + 16 * n + 8
    out = {}
    for _h, attr, a, b in nodes:
        off = names + (attr & 0xFFFF) * 4
        out[data[off:data.index(b"\0", off)].decode("ascii", "replace")] = data[data_off + a:data_off + b]
    return out


def rarc_files(data):
    """{name: bytes} of a RARC archive's files (names only; the archives here are shallow), or of a SARC."""
    if data[:4] == b"SARC":
        return sarc_bytes(data)
    data_off = struct.unpack(">I", data[0x0C:0x10])[0] + 0x20
    info = 0x20
    n_files = struct.unpack(">I", data[info + 0x08:info + 0x0C])[0]
    files_off = struct.unpack(">I", data[info + 0x0C:info + 0x10])[0] + info
    str_off = struct.unpack(">I", data[info + 0x14:info + 0x18])[0] + info
    out = {}
    for k in range(n_files):
        e = files_off + 0x14 * k
        idx, _hash, typ, name_off, off, size = struct.unpack(">HHHHII", data[e:e + 0x10])
        if idx == 0xFFFF or typ & 0x200:          # a directory
            continue
        name_end = data.index(b"\0", str_off + name_off)
        name = data[str_off + name_off:name_end].decode("ascii", "replace")
        out[name] = data[data_off + off:data_off + off + size]
    return out


def sarc_files(path):
    """{name: (path, start, end)} of a SARC pack's files."""
    with open(path, "rb") as f:
        head = f.read(0x14)
        hl, _bom, _size, data_off = struct.unpack(">HHII", head[4:16])
        f.seek(hl)
        sfat = f.read(12)
        _shl, n, _key = struct.unpack(">HHI", sfat[4:12])
        nodes = [struct.unpack(">IIII", f.read(16)) for _ in range(n)]
        sfnt = f.read(8)
        assert sfnt[:4] == b"SFNT"
        names = f.read(max(((attr & 0xFFFF) * 4 for _h, attr, _a, _b in nodes), default=0) + 256)
    out = {}
    for _h, attr, a, b in nodes:
        off = (attr & 0xFFFF) * 4
        name = names[off:names.index(b"\0", off)].decode("ascii", "replace")
        out[name] = (path, data_off + a, data_off + b)
    return out


def stage_files(stage):
    """{file name: a function returning its bytes} for STAGE's stage and room archives, loose or packed."""
    out = {}
    for path in glob.glob(os.path.join(PACKS, "*szs*.pack")):
        for name, (pack, a, b) in sarc_files(path).items():
            if name.startswith(stage + "_"):
                out[name] = (lambda pack=pack, a=a, b=b: open(pack, "rb").read()[a:b])
    for path in glob.glob(os.path.join(CONTENT, stage + "_*.szs")):
        out[os.path.basename(path)] = (lambda path=path: open(path, "rb").read())
    return out


def embedded_dz(blob):
    """The room.dzr / stage.dzs files WWHD embeds in a room's or stage's .bfres, found by their chunk table
    (u32 count, then count entries of tag[4], u32 count, u32 offset; offsets from the table's start)."""
    tag_ok = lambda b: len(b) == 4 and all(48 <= c <= 57 or 65 <= c <= 90 or 97 <= c <= 122 for c in b)   # ACTa, ACTb
    found = []
    for key in (b"ACTR", b"SCOB", b"TRES", b"PLYR"):
        at = blob.find(key)
        while at >= 0:
            q = at
            while q >= 16 and tag_ok(blob[q - 12:q - 8]):
                q -= 12
            start = q - 4
            n = struct.unpack(">I", blob[start:start + 4])[0] if start >= 0 else 0
            if 1 <= n <= 64 and all(tag_ok(blob[q + 12 * k:q + 12 * k + 4]) for k in range(n)) and start not in found:
                found.append(start)
            at = blob.find(key, at + 1)
    return [blob[start:] for start in found]


def actor_chunks(dz, full=False):
    """(tag, name) for every actor-placing entry of a dzr/dzs file (with full, also params, x, y, z, angle y)."""
    n = struct.unpack(">I", dz[0:4])[0]
    for k in range(n):
        tag, count, off = struct.unpack(">4sII", dz[4 + 12 * k:16 + 12 * k])
        tag = tag.decode("ascii", "replace")
        base = tag if tag in SIZES else {"ACT": "ACTR", "SCO": "SCOB", "TRE": "TRES"}.get(tag[:3])
        if base is None or (tag not in SIZES and not re.fullmatch(r"[0-9a-b]", tag[3])):
            continue
        size = SIZES[base]
        for j in range(count):
            e = off + size * j
            name = dz[e:e + 8].split(b"\0")[0].decode("ascii", "replace")
            if full:
                prm, x, y, z = struct.unpack(">Ifff", dz[e + 8:e + 0x18])
                ay = struct.unpack(">h", dz[e + 0x1A:e + 0x1C])[0]
                yield tag, name, prm, x, y, z, ay
            else:
                yield tag, name


def object_names():
    """{stage name: (WWHD process number, argument)} from l_objectName in the RPX's data."""
    rpx = Rpx(RPX)
    for i in range(len(rpx.sh)):
        if rpx.name(i) not in (".data", ".rodata"):
            continue
        data = rpx.data(i)
        at = data.find(b"kusax1\0\0")
        while at >= 0:
            if data[at + 12:at + 20] == b"kusax7\0\0":
                out = {}
                e = at
                while True:
                    raw = data[e:e + 8]
                    if not raw[:1].isalnum():
                        break
                    name = raw.split(b"\0")[0].decode("ascii", "replace")
                    proc, arg = struct.unpack(">hb", data[e + 8:e + 11])
                    out.setdefault(name, (proc, arg))
                    e += 12
                return out
            at = data.find(b"kusax1\0\0", at + 1)
    sys.exit("l_objectName not found in the RPX")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("stage")
    ap.add_argument("rooms", nargs="*")
    ap.add_argument("--names", default="/wwhd/data/ghidra-out/actor_names.tsv")
    ap.add_argument("--layers", action="store_true")
    ap.add_argument("--pos", type=int, help="print this process's placements")
    args = ap.parse_args()
    names = {}
    if os.path.exists(args.names):
        for line in open(args.names):
            p = line.rstrip("\n").split("\t")
            if p and p[0].isdigit():
                names[int(p[0])] = p[1]
    table = object_names()
    available = stage_files(args.stage)
    paths = []
    if (not args.rooms or "stage" in args.rooms) and f"{args.stage}_Stage.szs" in available:
        paths.append(("stage", available[f"{args.stage}_Stage.szs"]))
    rooms = sorted((int(m.group(1)), name) for name in available for m in [re.fullmatch(re.escape(args.stage) + r"_Room(\d+)\.szs", name)] if m)
    for room, name in rooms:
        if not args.rooms or str(room) in args.rooms:
            paths.append((str(room), available[name]))
    for room, load in paths:
        files = rarc_files(yaz0(load()))
        groups = collections.defaultdict(collections.Counter)
        for fname, blob in files.items():
            tables = [blob] if fname.endswith((".dzr", ".dzs")) else embedded_dz(blob) if fname.endswith(".bfres") else []
            if args.pos is not None:
                for dz in tables:
                    for tag, name, prm, x, y, z, ay in actor_chunks(dz, full=True):
                        if table.get(name, (-1, 0))[0] == args.pos:
                            print(f"{args.stage} room {room} {tag} {name} params {prm:08x} at {x:.0f},{y:.0f},{z:.0f} angle {ay}")
                continue
            for dz in tables:
                for tag, name in actor_chunks(dz):
                    groups[tag if args.layers else "all"][name] += 1
        for tag, counts in sorted(groups.items()):
            by_proc = collections.defaultdict(lambda: [0, set()])
            for name, n in counts.items():
                proc = table.get(name, (-1, 0))[0]
                by_proc[proc][0] += n
                by_proc[proc][1].add(name)
            items = ", ".join(f"{p} {names.get(p, '?')} x{n} ({'/'.join(sorted(s))})" for p, (n, s) in sorted(by_proc.items()))
            print(f"{args.stage} room {room}{'' if tag == 'all' else ' ' + tag}: {items}")


if __name__ == "__main__":
    main()
