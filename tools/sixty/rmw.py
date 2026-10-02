"""Read-modify-write sites in a function: the candidates for per-tick steps (D21).

Usage (on the worker):
    python3 tools/sixty/rmw.py ASM.s... [--all]

ASM.s is tools/ghidra/disasm.py's output for a function (one instruction a line). A site is a store
whose value, followed back through the instructions before it in its basic block, depends on a load
from the same address (same offset, same base register, the base unchanged in between): a field
that changes by its own previous value, as `x += (t - x) * k`, `timer--` or `v += g` do. Each is
printed with the chain of instructions that computes the stored value (constants loaded from
memory show as loads), so a rule (config/US_v0/tick_rules.txt) can be chosen: k@ on the factor of
an approach, *h on a per-tick amount, keep or whole on a counter. Integer stores whose chain is only
bit operations (flags) are left out unless --all. The output names the game's instructions: keep it
on the worker.
"""
import argparse
import re

BRANCH = re.compile(r"^(b|bc|bl|blr|bctr|bctrl|bdnz|bdz|beq|bne|blt|bgt|ble|bge|bso|bns)\b")
MEM = re.compile(r"^(-?0x[0-9a-f]+|-?\d+)\((r\d+)\)$")
STORE = re.compile(r"^(stb|sth|stw|stfs|stfd|psq_st)(u|x|ux)?$")
LOAD = re.compile(r"^(lbz|lhz|lha|lwz|lfs|lfd|psq_l)(u|x|ux)?$")
FLAGS = {"ori", "oris", "or", "and", "andi.", "andis.", "andc", "rlwinm", "rlwimi", "xor", "xori", "nor", "nand"}


def parse(path):
    """[(address, mnemonic, [operands])] and the branch targets."""
    out, targets = [], set()
    for line in open(path):
        m = re.match(r"^([0-9a-f]{8})\s+(\S+)\s*(.*)$", line.strip())
        if not m:
            continue
        ea, op, rest = int(m.group(1), 16), m.group(2), m.group(3).split("  ->")[0]
        ops = [o.strip() for o in rest.split(",")] if rest else []
        out.append((ea, op, ops))
        if BRANCH.match(op) and ops:
            t = ops[-1]
            if t.startswith("0x"):
                targets.add(int(t, 16))
    return out, targets


def regs_of(ops):
    return [o for o in ops if re.fullmatch(r"[rf]\d+", o)]


def defs_uses(op, ops):
    """(registers written, registers read, memory operand or None)."""
    mem = None
    for o in ops:
        m = MEM.match(o)
        if m:
            mem = (int(m.group(1), 0), m.group(2))
    rs = regs_of(ops)
    if STORE.match(op):
        uses = rs + ([mem[1]] if mem else [])
        defs = [mem[1]] if mem and op.endswith(("u", "ux")) else []
        return defs, uses, mem
    if LOAD.match(op):
        defs = rs[:1] + ([mem[1]] if mem and op.endswith(("u", "ux")) else [])
        return defs, (rs[1:] + ([mem[1]] if mem else [])), mem
    if op.startswith(("cmp", "fcmp")) or BRANCH.match(op) or op in ("mtspr", "mtcrf"):
        return [], rs, None
    if op == "mfspr":
        return rs[:1], [], None
    return rs[:1], rs[1:], None


def blocks(insns, targets):
    cur = []
    for ea, op, ops in insns:
        if ea in targets and cur:
            yield cur
            cur = []
        cur.append((ea, op, ops))
        if BRANCH.match(op) and op not in ("bl", "bctrl"):
            yield cur
            cur = []
    if cur:
        yield cur


def sites(block):
    found = []
    for k, (ea, op, ops) in enumerate(block):
        if not STORE.match(op):
            continue
        _, uses, mem = defs_uses(op, ops)
        if mem is None:
            continue
        value = regs_of(ops)[0]
        # follow the value back: which instructions compute it
        want, chain, loads_same = {value}, [], []
        for j in range(k - 1, -1, -1):
            e2, op2, ops2 = block[j]
            d2, u2, m2 = defs_uses(op2, ops2)
            if op2 in ("bl", "bctrl"):              # a call clobbers the volatile registers
                if want & {f"r{n}" for n in range(3, 13)} | {f"f{n}" for n in range(0, 14)}:
                    chain.append((e2, op2, ops2))
                break
            hit = set(d2) & want
            if not hit:
                continue
            chain.append((e2, op2, ops2))
            want = (want - hit) | set(u2) - ({m2[1]} if m2 and LOAD.match(op2) and m2[1] not in u2[:-1] else set())
            if LOAD.match(op2) and m2 == mem:
                # the base must be unchanged between this load and the store
                base_changed = any(mem[1] in defs_uses(o3, p3)[0] for _, o3, p3 in block[j + 1:k])
                if not base_changed:
                    loads_same.append(e2)
        if loads_same:
            found.append((ea, op, ops, list(reversed(chain))))
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("asm", nargs="+")
    ap.add_argument("--all", action="store_true", help="flag updates too")
    args = ap.parse_args()
    for path in args.asm:
        insns, targets = parse(path)
        print(f"// {path}")
        for b in blocks(insns, targets):
            for ea, op, ops, chain in sites(b):
                kinds = {o for _, o, _ in chain}
                if not args.all and op in ("stw", "sth", "stb") and kinds - {"lwz", "lhz", "lha", "lbz"} <= FLAGS:
                    continue
                print(f"{ea:08x}  {op} {','.join(ops)}")
                for e2, o2, p2 in chain:
                    print(f"      {e2:08x}  {o2} {','.join(p2)}")
        print()


if __name__ == "__main__":
    main()
