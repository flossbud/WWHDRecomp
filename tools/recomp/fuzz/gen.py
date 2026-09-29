"""Generate the instruction-fuzzer cases: build/fuzz/cases.cpp (docs/recompiler-design.md D8.1).

Usage:
    python3 tools/recomp/fuzz/gen.py OUT.cpp [--per-op N] [--ops mnemonics.csv] [--seed S]

For every mnemonic (or those listed in --ops, e.g. the census of the game), synthesise N random
encodings (random operand fields, no game data) and emit one C++ function per encoding, made by
emit.py in single-step mode, plus a table the harness walks. Encodings the harness can't run
safely are skipped: address forms that would leave the test memory window (rA=0 with a
displacement, update forms with rA=0 or rA=rD, lmw/lswi overlapping rA), SPRs other than the ones
WWHD uses, and ops whose Cemu handler needs a running emulator (dcbf/dcbst/icbi, tw, sc, rfi, ...).
"""
import csv
import pathlib
import random
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import emit  # noqa: E402
import ppc  # noqa: E402

EA = 0x02001000                   # guest address of the instruction under test (harness agrees)
SKIP = {"dcbf", "dcbst", "icbi", "tw", "twi", "sc", "rfi", "mfmsr", "mtmsr", "mtsr", "mtsrin",
        "mfsr", "mfsrin", "tlbie", "tlbsync", "eciwx", "ecowx", "mftb", "mcrxr", "lswx", "stswx",
        "mtfsb0", "mtfsb1", "mtfsf", "mtfsfi", "mcrfs", "fcmpo", "fsqrt", "fsqrts", "ps_cmpo1"}
SPRS = [1, 8, 9] + list(range(896, 904))

# memory-access kinds the harness must aim into its window: (indexed, has_displacement)
MEM_D = {"lwz", "lwzu", "lbz", "lbzu", "lhz", "lhzu", "lha", "lhau", "stw", "stwu", "stb", "stbu",
         "sth", "sthu", "lmw", "stmw", "lfs", "lfsu", "lfd", "lfdu", "stfs", "stfsu", "stfd", "stfdu",
         "psq_l", "psq_lu", "psq_st", "psq_stu"}
MEM_X = {"lwzx", "lwzux", "lbzx", "lbzux", "lhzx", "lhzux", "lhax", "lhaux", "stwx", "stwux", "stbx",
         "stbux", "sthx", "sthux", "lwbrx", "lhbrx", "stwbrx", "sthbrx", "lfsx", "lfsux", "lfdx",
         "lfdux", "stfsx", "stfsux", "stfdx", "stfdux", "stfiwx", "psq_lx", "psq_stx", "psq_lux",
         "psq_stux", "lwarx", "stwcx.", "dcbz", "dcbt", "dcbtst", "dcbi", "dcbz_l"}
MEM_NB = {"lswi", "stswi"}        # (rA|0) only
KIND = {**{o: 1 for o in MEM_D}, **{o: 2 for o in MEM_X}, **{o: 3 for o in MEM_NB}}


def acceptable(i):
    op, f = i.op, i.f
    if op in SKIP or f.get("oe"):             # WWHD has no OE=1 forms (census)
        return False
    if op == "fcmpu" and ppc.bits(i.word, 9, 2):   # reserved bits: Cemu then writes a misaligned CR field
        return False
    if op in ("mfspr", "mtspr") and f["spr"] not in SPRS:
        return False
    if op in MEM_D or op in MEM_NB:
        if f["rA"] == 0:
            return False
    if op in MEM_X and f["rA"] == f["rB"]:
        return False
    upd = op in KIND and (op.endswith("u") or op.endswith("ux"))
    if upd:
        if f["rA"] == 0 or f["rA"] == f.get("rT", -1):
            return False
    if op == "lmw" and f["rA"] >= f["rT"]:
        return False
    if op in MEM_NB:
        nregs = ((f["nb"] or 32) + 3) // 4
        if any((f["rT"] + k) % 32 == f["rA"] for k in range(nregs)):
            return False
    return True


def encodings(per_op, only, rng):
    """Random words grouped by mnemonic: sample random low bits under every primary opcode."""
    found = {}
    for p in range(64):
        for _ in range(60000 if p in (4, 19, 31, 59, 63) else 4000):
            w = (p << 26) | rng.getrandbits(26)
            i = ppc.decode(w)
            if i is None or (only and i.op not in only) or not acceptable(i):
                continue
            lst = found.setdefault(i.op, [])
            if len(lst) < per_op:
                lst.append(i)
    # SPR moves: random sampling almost never hits the few SPR numbers allowed, so build them
    for op, xo in (("mfspr", 339), ("mtspr", 467)):
        if only and op not in only:
            continue
        found[op] = []
        for k in range(per_op):
            spr = SPRS[k % len(SPRS)]
            w = (31 << 26) | (rng.randrange(32) << 21) | ((spr & 31) << 16) | ((spr >> 5) << 11) | (xo << 1)
            found[op].append(ppc.decode(w))
    return found


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    out = pathlib.Path(args[0])
    per_op = int(args[args.index("--per-op") + 1]) if "--per-op" in args else 64
    seed = int(args[args.index("--seed") + 1]) if "--seed" in args else 1
    only = None
    if "--ops" in args:
        with open(args[args.index("--ops") + 1]) as f:
            only = {r["mnemonic"] for r in csv.DictReader(f)}
    rng = random.Random(seed)
    found = encodings(per_op, only, rng)
    em = emit.Emitter(emit.StepFlow())
    body, table, unsupported = [], [], set()
    for op in sorted(found):
        for k, i in enumerate(found[op]):
            name = f"c_{op.replace('.', '_')}_{k}"
            try:
                lines = em.emit(i, EA)
            except NotImplementedError:
                unsupported.add(op)
                continue
            body.append(f"static void {name}(PPCInterpreter_t* ctx)\n{{")
            body += ["\t" + l for l in lines]
            body.append("}\n")
            f = i.f
            table.append(f'\t{{"{op}", 0x{i.word:08X}u, {name}, {KIND.get(op, 0)}, {f.get("rA", 0)}, '
                         f'{f.get("rB", 0)}, {f.get("d", 0)}}},')
    missing = sorted((only or set()) - set(found) - SKIP)
    if unsupported:
        print("emitter lacks: " + " ".join(sorted(unsupported)))
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("// Generated by tools/recomp/fuzz/gen.py - do not edit, do not commit.\n"
                   '#include "ppc_ops.h"\n#include "fuzz.h"\n\n' + "\n".join(body)
                   + "\nconst FuzzCase g_cases[] = {\n" + "\n".join(table) + "\n};\n"
                   + f"const size_t g_caseCount = {len(table)};\n"
                   + f"const uint32 g_caseEA = 0x{EA:08X}u;\n")
    print(f"{len(table)} cases over {len(found)} mnemonics -> {out}")
    if missing:
        print("no encodings for: " + " ".join(missing))
    skipped = sorted((only or set()) & SKIP)
    if skipped:
        print("not fuzzed (need a running emulator): " + " ".join(skipped))


if __name__ == "__main__":
    main()
