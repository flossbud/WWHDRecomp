"""Whole-program generator (docs/recompiler-design.md D1, D4, D5, D7; milestone M2).

Usage:
    python3 tools/recomp/generate.py orig/0005000010143500_v0/code/cking.rpx OUT_DIR [--per-shard 256] [--tick instruction]

Writes OUT_DIR/shard_NNN.cpp (one C++ function per guest function in functions.csv), funcs.h
(declarations), func_table.cpp ({guest address, host function}) and imports.cpp (the import
table the runtime binds to Cemu's HLE handlers). Output is generated from game code: it goes to
build/ and is never committed. A shard is rewritten only when its content changes.

config/US_v0/code_patches.csv (D10) is applied first: the boot-time patches Cemu's GamePatch makes to
this RPX, so the generated code is the code the interpreter runs.

func_table.cpp also carries, per function, its end, a hash of its code as the RPX has it (so the
runtime can tell which functions Cemu patched in memory, D10) and flags. A function is *pure*
(D8.2) if it and everything it can call make no import call, no indirect call or jump, and no
call to a weak (address 0) symbol; diff mode (M3) checks pure functions against the interpreter.
imports.cpp also lists every import relocation site, so the runtime can bind and cross-check
each import against what Cemu's loader wrote into guest memory, and a census of store
instructions that the runtime's store decoder (diff mode) must reproduce.

Every instruction costs one cycle of the timeslice, and the thread yields in place, before the
instruction at which the slice runs out (D6, as revised for M4). Counted per basic block: a block
(from a label or the instruction after a branch, call, import or runtime hook, to the next) of at
least 3 instructions that fits in what is left of the slice is charged at once (RT_FITS/RT_CHARGE)
and runs without checks; one that doesn't fit runs a second copy with RT_TICK(address) before every
instruction, so the yield lands on the same instruction. Nothing inside a block can tell the
difference: a block ends before anything that could look at the budget (calls, imports, the
runtime's hooks), and guest time only advances when a slice ends. --tick instruction emits only
the checked form (one RT_TICK per instruction), as before.

Control flow (D1): branches inside a function become gotos; a branch to another function's
entry is a musttail call; bl is a call; blr is return; a bctr listed in jump_tables.csv is a
switch on CTR; other bctr/bctrl go through the runtime's function table. A function whose last
instruction can fall through tail-calls the next function, which is also how the GHS save/restore
helpers (D7) chain from entry to entry. Branches to imports (REL24 relocations into .fimport_*)
call the HLE import; immediates relocated against data imports (.dimport_*) are read from the
runtime. Anything the generator cannot place is an error, listed at the end; exit status 1.

Overrides (D9): a function listed in config/US_v0/overrides.txt is emitted as orig_f_X, and f_X is
left to src/overrides, which defines it (and may call orig_f_X). Every reference (direct and tail
calls, falling through, the function table, and with it the runtime's indirect calls) names f_X, so
all of them reach the override; the linker reports a listed function without an override, and an
override of a function that isn't listed, as a missing or duplicate symbol.

Tick rules (D21): an instruction listed in config/US_v0/tick_rules.txt runs only on the game's
whole ticks when it runs 60 frames a second: its code is wrapped in `if (RT_WHOLE_TICK())`, which
is always true at 30 fps, so nothing changes there. A rule names the instruction it expects (a call:
`bl TARGET` or `bctrl`; a store without update: its mnemonic), and a different instruction at that
address is an error. `whole:r3=N` also sets r3 to N when a call is skipped, for callers that test
its result, `whole:r3=rN` sets it to the register rN (a call's own argument: "unchanged"). `late`
runs it once a tick at the tick's end instead: on the half tick while its process steps at 60 (a
tick counter, so that both frames of a tick see the tick's count, as `1 / (N - count)` approaches
need; RT_LATE_TICK), on the whole tick otherwise; it takes the same instructions and r3=. Step
rules, for the code of processes that run every frame with a time step h
(g_rtStep, src/overrides/sixty.cpp; nothing changes while it is 1, at 30 fps always):
  keep:SRC     on a half tick the instruction's destination gets SRC instead (a counter that
               counts whole ticks: `addi r0, r3, 1` with keep:r3)
  *h:REG /h:REG  after the instruction, REG times or divided by h (a per-tick amount; a distance
               per step that should read per tick)
  k:REG        after it, REG = 1 - (1 - REG)^h (an exponential approach's factor)
  k75:REG      an approach whose result is then approached by 0.75 (k@ on that one): REG becomes
               approach(0.75 REG) / approach(0.75), so the two together approach as one tick does
  d:REG        after it, REG = REG^h (a damping factor)
  split:REG    after it, an integer per-tick amount split between the whole tick and the half tick
               (REG - REG/2, then REG/2: the two add up to the 30 Hz step exactly)
  OP@REG       the same, for this instruction only: REG has its value back afterwards (unless the
               instruction writes it), as in `x += (t - x) * k` with k@f2 on its fmadds
  note:REG     after the instruction, the float REG is noted (g_rtNote) for an arc@ later in the step
  vec@rN       for a call, the vector (three floats) rN points to is h of itself (a per-tick move
               handed to a vector add)
  arc@rN       the same for a velocity: x and z times h, y as h y + (1 - h)/2 (y - noted), noted
               before gravity was added: the semi-implicit 30 Hz arc exactly at whole ticks
  reload:fD=rB+O[+O2]  after the instruction, at 60 fps (g_rtSixty, drawing too, where the step
               is 1), fD is the float at rB + O again (with O2, at the word at rB + O, plus O2):
               code that truncates a frame count kept as a float (whose half steps are .5) gets it
               whole (the particles' texture scroll, `int tick = getFrame()`)
REG is a register (r3, f1). These name any instruction by its mnemonic (`bl TARGET` for a call).
"""
import bisect
import collections
import re
import csv
import hashlib
import pathlib
import struct
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))
import emit  # noqa: E402
import ppc  # noqa: E402
from rpx_info import Rpx  # noqa: E402

ROOT = HERE.parents[1]
CONFIG = ROOT / "config/US_v0"
REL24, ADDR16_LO, ADDR16_HI, ADDR16_HA = 10, 4, 5, 6
GHS_HELPERS = (0x028F5EE0, 0x028F626C)   # GHS register save/restore runs (D7)
# instructions that write guest memory (the runtime's diff-mode store decoder must know them all)
STORES = {"stw", "stwu", "stwx", "stwux", "stb", "stbu", "stbx", "stbux", "sth", "sthu", "sthx", "sthux",
          "stmw", "stswi", "stswx", "stwbrx", "sthbrx", "stfs", "stfsu", "stfsx", "stfsux", "stfd", "stfdu",
          "stfdx", "stfdux", "stfiwx", "psq_st", "psq_stu", "psq_stx", "psq_stux", "stwcx.", "dcbz"}


class Program:
    def __init__(self, rpx_path):
        rpx = Rpx(rpx_path)
        self.sections = [(rpx.name(i), s[3], rpx.size(i)) for i, s in enumerate(rpx.sh) if s[3]]
        # code: .text, plus .syscall (an 8-byte "nop; blr" stub that some code calls)
        self.code = [(rpx.sh[i][3], rpx.data(i)) for i in range(len(rpx.sh)) if rpx.name(i) in (".text", ".syscall")]
        # D10: the boot-time patches Cemu applies to this RPX (GamePatch.cpp), so the generated
        # code is what the interpreter runs; the runtime's hash check at boot confirms it
        self.patches = {}
        with open(CONFIG / "code_patches.csv") as f:
            for r in csv.DictReader(f):
                ea, orig, new = int(r["address"], 16), int(r["original"], 16), int(r["patched"], 16)
                assert self.word(ea) == orig, f"code_patches.csv: {ea:08X} holds {self.word(ea):08X}, not {orig:08X}"
                self.patches[ea] = new
        # imports: symbols defined in .fimport_LIB (functions) and .dimport_LIB (data)
        self.imports, self.import_is_data = {}, {}
        for i, s in enumerate(rpx.sh):
            if s[1] != 2:                                  # SYMTAB
                continue
            syms, strtab = rpx.data(i), rpx.data(s[6])
            for k in range(0, len(syms), 16):
                name_off, value, _size, info, _other, shndx = struct.unpack_from(">IIIBBH", syms, k)
                if info & 0xF == 3:                        # STT_SECTION: shares the first import's address
                    continue
                if 0 < shndx < len(rpx.sh) and rpx.name(shndx).startswith((".fimport_", ".dimport_")):
                    name = strtab[name_off:strtab.index(b"\0", name_off)].decode()
                    self.imports[value] = (rpx.name(shndx).split("_", 1)[1], name)
                    self.import_is_data[value] = rpx.name(shndx).startswith(".dimport_")
        # functions (the IMPORTED rows of functions.csv are the .fimport stubs, handled above)
        self.funcs = []
        with open(CONFIG / "functions.csv") as f:
            for r in csv.DictReader(f):
                a, e = int(r["address"], 16), int(r["end"], 16)
                if r["source"] != "IMPORTED" and self.in_text(a):
                    self.funcs.append((a, e, r["name"]))
        self.funcs.sort()
        self.entries = {a for a, _, _ in self.funcs}
        self.starts = [a for a, _, _ in self.funcs]
        self.import_ids = {a: n for n, a in enumerate(sorted(self.imports))}
        # relocations patched into .text: branch targets and relocated immediates
        # (by symbol: _iob+0x10 lands on environ's stub address but means _iob's second FILE)
        self.rel24, self.imm_import = {}, {}
        for sec, off, typ, value, addend in rpx.relocations_by_symbol():
            if sec != ".text":
                continue
            if typ == REL24:
                assert not (value in self.imports and addend), (hex(off), "branch to an import plus an addend")
                self.rel24[off] = (value + addend) & 0xFFFFFFFF
            elif typ in (ADDR16_LO, ADDR16_HI, ADDR16_HA) and value in self.imports:
                self.imm_import[off & ~3] = (typ, value, addend)
        # bctr -> the values CTR can hold there, which the switch cases on. If the table is a run
        # of b instructions in the code (all 294 of WWHD's are, whatever the csv's "bound" column
        # says about how the bound was found), the bctr jumps into it: CTR holds a slot's address
        # (slot k at table + 4k), and the slot's own b goes on to the listed target. A table of
        # addresses in data would hold the targets themselves.
        self.jump_tables = {}
        with open(CONFIG / "jump_tables.csv") as f:
            for r in csv.DictReader(f):
                base, n = int(r["table"], 16), int(r["count"])
                targets = [int(t, 16) for t in r["targets"].split()]
                slots = [base + 4 * k for k in range(n)]
                if self.in_text(base) and all(self._is_b(a) for a in slots):
                    assert [self._b_target(a) for a in slots] == targets, r["bctr"]
                    self.jump_tables[int(r["bctr"], 16)] = slots
                else:
                    self.jump_tables[int(r["bctr"], 16)] = targets
        self.names = {}
        sym = CONFIG / "symbols.csv"
        if sym.exists():
            with open(sym) as f:
                for r in csv.DictReader(f):
                    try:
                        self.names[int(r["address"], 16)] = r["name"]
                    except (KeyError, ValueError):
                        pass
        # D9: functions replaced by src/overrides (their generated bodies become orig_f_X)
        self.overrides = set()
        with open(CONFIG / "overrides.txt") as f:
            for line in f:
                word = line.split("#", 1)[0].split()
                if word:
                    a = int(word[0], 16)
                    assert a in self.entries, f"overrides.txt: {a:08X} is not a function entry"
                    self.overrides.add(a)
        # D21: instructions that run only on whole ticks at 60 fps
        self.tick_rules = self.load_tick_rules(CONFIG / "tick_rules.txt")
        self.synthetic = self.helper_entries()
        if self.synthetic:
            self.funcs = sorted(self.funcs + self.synthetic)
            self.entries |= {a for a, _, _ in self.synthetic}

    STEP_OPS = {"*h": "rt_step_mul", "/h": "rt_step_div", "k": "rt_step_approach", "d": "rt_step_damp",
                "k75": "rt_step_approach75"}

    def load_tick_rules(self, path):
        """address -> (rule, argument, expected instruction text, what): see the docstring. rule is
        whole (argument: r3's value or None), keep (argument: the source register), split or one
        of STEP_OPS (argument: the register)."""
        rules = {}
        if not path.exists():
            return rules
        with open(path) as f:
            for n, line in enumerate(f, 1):
                words = line.split("#", 1)[0].split()
                if not words:
                    continue
                assert len(words) >= 3, f"tick_rules.txt:{n}: address, rule and instruction expected"
                # the instruction is `bl TARGET`, `bctrl` or a store's mnemonic; what follows says what it is
                n_insn = 2 if words[2] == "bl" else 1
                ea, rule, expect = int(words[0], 16), words[1], " ".join(words[2:2 + n_insn])
                what = " ".join(words[2 + n_insn:])
                kind, sep, arg = rule.partition(":")
                if "@" in rule:
                    kind, sep, arg = rule.partition("@")
                    assert kind in ("split", "vec", "arc") or kind in self.STEP_OPS, f"tick_rules.txt:{n}: {rule}: @ takes a step operation"
                    assert kind not in ("vec", "arc") or (arg[0] == "r" and words[2] == "bl"), f"tick_rules.txt:{n}: {rule}: a call's pointer register"
                    kind += "@"
                if kind == "reload":
                    m = re.fullmatch(r"(f(?:[12]?[0-9]|3[01]))=(r(?:[12]?[0-9]|3[01]))\+(0x[0-9a-fA-F]+)(?:\+(0x[0-9a-fA-F]+))?", arg)
                    assert m, f"tick_rules.txt:{n}: {rule}: reload:fD=rB+OFFSET[+OFFSET] expected"
                    value = (m.group(1), m.group(2), int(m.group(3), 16), None if m.group(4) is None else int(m.group(4), 16))
                    assert self.function_containing(ea) is not None, f"tick_rules.txt:{n}: {ea:08X} is in no function"
                    assert ea not in rules, f"tick_rules.txt:{n}: {ea:08X} listed twice"
                    rules[ea] = (kind, value, expect, what)
                    continue
                if kind in ("whole", "late"):
                    value = None
                    if arg:
                        key, _, v = arg.partition("=")
                        assert key == "r3", f"tick_rules.txt:{n}: unknown rule argument {arg}"
                        value = v if re.fullmatch(r"r([12]?[0-9]|3[01])", v) else int(v, 0)
                else:
                    assert kind.rstrip("@") in ("keep", "split", "note", "vec", "arc") or kind.rstrip("@") in self.STEP_OPS, f"tick_rules.txt:{n}: unknown rule {rule}"
                    assert kind != "note" or arg[0] == "f", f"tick_rules.txt:{n}: note takes a float register"
                    assert re.fullmatch(r"[rf]([12]?[0-9]|3[01])", arg), f"tick_rules.txt:{n}: {rule}: a register expected"
                    assert kind.rstrip("@") != "split" or arg[0] == "r", f"tick_rules.txt:{n}: split takes an integer register"
                    value = arg
                assert self.function_containing(ea) is not None, f"tick_rules.txt:{n}: {ea:08X} is in no function"
                assert ea not in rules, f"tick_rules.txt:{n}: {ea:08X} listed twice"
                rules[ea] = (kind, value, expect, what)
        return rules

    def check_tick_rule(self, ea, i):
        """The instruction a tick rule expects at ea, or an error message."""
        kind, r3, expect, _ = self.tick_rules[ea]
        if kind not in ("whole", "late"):
            have = i.op
            if i.op == "b" and i.lk:
                have = f"bl {self.rel24.get(ea, (ea + i.li) & 0xFFFFFFFF):08x}"
            elif i.op in ("b", "bc", "bclr", "bcctr"):
                return f"{ea:08X}: step rules don't apply to branches ({i.op})"
            if have != expect.lower():
                return f"{ea:08X}: step rule expects `{expect}`, the code has `{have}`"
            if kind == "keep" and i.op not in KEEP_OPS:
                return f"{ea:08X}: keep applies to {', '.join(sorted(KEEP_OPS))}, not {i.op}"
            return None
        if i.op == "b" and i.lk:
            target = self.rel24.get(ea, (ea + i.li) & 0xFFFFFFFF)
            have = f"bl {target:08x}"
        elif i.op == "bcctr" and i.lk and (i.bo & 0x14) == 0x14:
            have = "bctrl"
        elif i.op in STORES and not i.op.endswith(("u", "ux")) and i.op not in ("stwcx.", "stmw", "stswi"):
            have = i.op
        else:
            return f"{ea:08X}: tick rules apply to calls and plain stores, not {i.op}"
        if have != expect.lower():
            return f"{ea:08X}: tick rule expects `{expect}`, the code has `{have}`"
        if r3 is not None and not have.startswith(("bl", "bctrl")):
            return f"{ea:08X}: r3= only applies to calls"
        return None

    def helper_entries(self):
        """D7: epilogues branch into the middle of the GHS save/restore runs. Each such target
        becomes its own function: a copy of the run from there to the end of its function."""
        targets = set()
        for start, end, _ in self.funcs:
            for ea in range(start, end, 4):
                i = ppc.decode(self.word(ea))
                if i is None or i.op not in ("b", "bc") or ea in self.rel24:
                    continue
                t = (ea + (i.li if i.op == "b" else i.bd)) & 0xFFFFFFFF
                if GHS_HELPERS[0] <= t < GHS_HELPERS[1] and t not in self.entries and not start <= t < end:
                    targets.add(t)
        out = []
        for t in sorted(targets):
            host = self.function_containing(t)
            out.append((t, host[1], f"ghs_helper_{t:08X}"))
        return out

    def _is_b(self, ea):
        i = ppc.decode(self.word(ea))
        return i is not None and i.op == "b" and not i.lk and not i.aa

    def _b_target(self, ea):
        return (ea + ppc.decode(self.word(ea)).li) & 0xFFFFFFFF

    def section_of(self, a):
        for name, base, size in self.sections:
            if base <= a < base + size:
                return name
        return "?"

    def in_text(self, a):
        return any(base <= a < base + len(data) for base, data in self.code)

    def word(self, ea):
        if ea in getattr(self, "patches", ()):
            return self.patches[ea]
        for base, data in self.code:
            if base <= ea < base + len(data):
                return struct.unpack_from(">I", data, ea - base)[0]
        raise IndexError(f"{ea:08X} is not code")

    def function_containing(self, a):
        i = bisect.bisect_right(self.starts, a) - 1
        if i >= 0 and self.funcs[i][0] <= a < self.funcs[i][1]:
            return self.funcs[i]
        return None


def fname(a):
    return f"f_{a:08X}"


class FunctionFlow:
    """Control flow for one whole function (see the module docstring)."""

    def __init__(self, prog, start, end, errors):
        self.prog, self.start, self.end, self.errors = prog, start, end, errors
        self.labels = set()
        self.callees = set()       # functions this one calls or tail-calls (incl. falling through)
        self.impure = []           # why it is not pure by itself (D8.2)

    def fallthrough(self, ea):
        return []

    @staticmethod
    def _wrap(cond, body):
        if cond is None:
            return ["{"] + ["\t" + b for b in body] + ["}"]
        return [f"if ({cond}) {{"] + ["\t" + b for b in body] + ["}"]

    def branch_to(self, ea, target, cond, link):
        p = self.prog
        if ea in p.rel24:                                  # relocated: import or weak symbol
            target = p.rel24[ea]
        if target in p.imports:
            self.impure.append("import")
            body = [f"rt_import(ctx, {p.import_ids[target]}); // {'.'.join(p.imports[target])}"]
            if link:
                body.insert(0, f"ctx->spr.LR = {emit.hx(ea + 4)};")
            else:
                body.append("return;")                    # tail call to the import
            return self._wrap(cond, body)
        if target == 0:
            self.impure.append("weak")
            return self._wrap(cond, [f"rt_bad_branch(ctx, {emit.hx(ea)}, 0u); return;"])
        if link:
            if target == ea + 4 and target < self.end:      # "bl $+4": reads the PC into LR
                return self._wrap(cond, [f"ctx->spr.LR = {emit.hx(ea + 4)};"])
            if target not in p.entries:
                self.errors.append(f"{ea:08X}: call to {target:08X}, not a function entry")
                self.impure.append("error")
                return self._wrap(cond, [f"rt_bad_branch(ctx, {emit.hx(ea)}, {emit.hx(target)}); return;"])
            self.callees.add(target)
            return self._wrap(cond, [f"ctx->spr.LR = {emit.hx(ea + 4)};", f"{fname(target)}(ctx);"])
        if self.start <= target < self.end:
            self.labels.add(target)
            return [f"goto L_{target:08X};"] if cond is None else [f"if ({cond}) goto L_{target:08X};"]
        if target in p.entries:
            self.callees.add(target)
            return self._wrap(cond, [f"[[clang::musttail]] return {fname(target)}(ctx);"])
        self.errors.append(f"{ea:08X}: branch to {target:08X}, neither in the function nor an entry")
        self.impure.append("error")
        return self._wrap(cond, [f"rt_bad_branch(ctx, {emit.hx(ea)}, {emit.hx(target)}); return;"])

    def branch_lr(self, ea, cond, link):
        if link:
            self.errors.append(f"{ea:08X}: bclrl")
        return ["return;"] if cond is None else [f"if ({cond}) return;"]

    def branch_ctr(self, ea, cond, link):
        if link:
            self.impure.append("bctrl")
            return self._wrap(cond, [f"ctx->spr.LR = {emit.hx(ea + 4)};", "RT_CALL_CTR();"])
        table = self.prog.jump_tables.get(ea)
        if table is None:
            self.impure.append("bctr")
            return self._wrap(cond, ["RT_JUMP_CTR();"])
        body = ["switch (ctx->spr.CTR & ~3u) {"]
        for t in sorted(set(table)):
            if self.start <= t < self.end:
                self.labels.add(t)
                body.append(f"case {emit.hx(t)}: goto L_{t:08X};")
            else:
                self.errors.append(f"{ea:08X}: jump table target {t:08X} outside the function")
        body += [f"default: rt_bad_branch(ctx, {emit.hx(ea)}, ctx->spr.CTR); return;", "}"]
        return self._wrap(cond, body)


def is_terminator(i):
    if i.op == "b":
        return not i.lk
    if i.op in ("bclr", "bcctr"):
        return (i.bo & 0x14) == 0x14 and not i.lk
    return False


def relocated(prog, i, ea):
    """Replace immediates relocated against data imports with the runtime's value."""
    typ, tgt, addend = prog.imm_import[ea]
    expr = f"rt_import_data({prog.import_ids[tgt]})" + (f" + {addend:#x}u" if addend else "")
    if typ == ADDR16_HA:
        assert i.op == "addis", (hex(ea), i.op)
        return f"GPR({i.rD}) = {emit.ra0(i)} + ((({expr}) + 0x8000u) & 0xFFFF0000u);"
    if typ == ADDR16_HI:
        assert i.op == "addis", (hex(ea), i.op)
        return f"GPR({i.rD}) = {emit.ra0(i)} + (({expr}) & 0xFFFF0000u);"
    f = dict(i.f)
    lo = f"(uint32)(sint32)(sint16)(({expr}) & 0xFFFFu)"
    if "simm" in f:
        f["simm"] = lo
    elif "d" in f:
        f["d"] = lo
    else:
        raise AssertionError((hex(ea), i.op))
    return ppc.Insn(i.op, i.word, f)


# an instruction ends its basic block (for cycle counting) if it may branch, call or return, or calls
# into the runtime, which could look at the timeslice
BLOCK_END_OPS = {"b", "bc", "bclr", "bcctr"}
BLOCK_END_CODE = ("rt_import(", "RT_CALL_CTR(", "RT_JUMP_CTR(", "rt_bad_branch(", "rt_trap(", "rt_dcache_flush(",
                  "return", "goto ", "switch (")
MIN_BLOCK = 3            # shorter blocks keep one RT_TICK per instruction
TICK_PER_INSTRUCTION = False


def ends_block(op, lines):
    return op in BLOCK_END_OPS or any(c in l for l in lines for c in BLOCK_END_CODE) or \
        any(re.search(r"\bf_[0-9A-F]{8}\(ctx\)", l) for l in lines)


def emit_blocks(body, labels):
    """body: [(ea, op, lines)] of one function, in order; returns its C++ lines, counted per block."""
    blocks, cur = [], []
    for ea, op, lines in body:
        if cur and ea in labels:
            blocks.append(cur)
            cur = []
        cur.append((ea, lines))
        if ends_block(op, lines):
            blocks.append(cur)
            cur = []
    if cur:
        blocks.append(cur)
    out = []
    for block in blocks:
        if block[0][0] in labels:
            out.append(f"L_{block[0][0]:08X}:;")
        if TICK_PER_INSTRUCTION or len(block) < MIN_BLOCK:
            for ea, lines in block:
                out.append(f"\tRT_TICK({emit.hx(ea)});")
                out += ["\t" + l for l in lines]
            continue
        out.append(f"\tif (RT_FITS({len(block)})) [[likely]] {{")
        out.append(f"\t\tRT_CHARGE({len(block)});")
        for ea, lines in block:
            out += ["\t\t" + l for l in lines]
        out.append("\t} else {")
        for ea, lines in block:
            out.append(f"\t\tRT_TICK({emit.hx(ea)});")
            out += ["\t\t" + l for l in lines]
        out.append("\t}")
    return out


KEEP_OPS = {"addi", "addic", "add", "fadds", "fsubs", "fadd", "fsub"}


def reg_expr(reg):
    return f"GPR({reg[1:]})" if reg[0] == "r" else f"FPR({reg[1:]})"


def apply_tick_rule(rule, i, lines):
    """An instruction's generated lines with its tick or step rule (see the docstring)."""
    kind, arg, _, what = rule
    if kind in ("whole", "late"):
        test = "RT_WHOLE_TICK()" if kind == "whole" else "RT_LATE_TICK()"
        out = [f"if ({test}) {{   // tick rule: {what}"] + ["\t" + l for l in lines] + ["}"]
        if arg is not None:
            out += ["else", f"\tGPR(3) = {reg_expr(arg) if isinstance(arg, str) else emit.hx(arg)};"]
        return out
    if kind == "reload":
        dest, base, off, off2 = arg
        ea = f"GPR({base[1:]}) + {emit.hx(off)}"
        if off2 is not None:
            ea = f"rd32({ea}) + {emit.hx(off2)}"
        d = reg_expr(dest)
        return lines + [f"if (RT_SIXTY()) {{ const uint32 w_ = rd32({ea}); float f_; memcpy(&f_, &w_, 4); "
                        f"{d}.fp0 = (double)f_; {d}.fp1 = {d}.fp0; }}   // step rule: {what}"]
    if kind == "keep":
        dest = f"GPR({i.rD})" if i.op in ("addi", "addic", "add") else f"FPR({i.frD})"
        return ([f"if (RT_WHOLE_TICK()) {{   // step rule: {what}"] + ["\t" + l for l in lines]
                + ["} else {", f"\t{dest} = {reg_expr(arg)};", "}"])
    reg = reg_expr(arg)
    op = kind.rstrip("@")
    if op == "note":
        return lines + [f"if (RT_STEPPED()) g_rtNote = (float){reg}.fp0;   // step rule: {what}"]
    if op in ("vec", "arc"):
        arc = "true" if op == "arc" else "false"
        return ([f"{{ const uint32 vecEa_ = {reg}; float vecSaved_[3]; const bool vec_ = RT_STEPPED();   // step rule: {what}",
                 f"\tif (vec_) rt_step_vec_begin(vecEa_, vecSaved_, {arc});"]
                + ["\t" + l for l in lines] + ["\tif (vec_) rt_step_vec_end(vecEa_, vecSaved_);", "}"])
    if op == "split":
        change = f"if (RT_STEPPED()) {reg} = rt_step_split({reg});"
    elif arg[0] == "f":
        fn = Program.STEP_OPS[op]
        change = f"if (RT_STEPPED()) {{ {reg}.fp0 = {fn}({reg}.fp0); {reg}.fp1 = {reg}.fp0; }}"
    else:
        fn = Program.STEP_OPS[op]
        change = f"if (RT_STEPPED()) {reg} = (uint32)(sint32){fn}((double)(sint32){reg});"
    if not kind.endswith("@"):
        return lines + [f"{change}   // step rule: {what}"]
    # for this instruction only: the register gets its value back unless the instruction wrote it
    assign = re.compile(re.escape(reg) + r"(\.fp[01](int)?)?\s*=(?!=)")
    call = (i.op == "b" or i.op == "bcctr") and i.lk           # a call: the argument registers are the callee's
    writes = call or any(assign.search(l) for l in lines)     # (or the instruction's own code assigns it)
    out = [f"{{ const auto saved_ = {reg}; {change}   // step rule: {what}"] + ["\t" + l for l in lines]
    return out + (["}"] if writes else [f"\t{reg} = saved_;", "}"])


def generate_function(prog, em, start, end, errors):
    flow = FunctionFlow(prog, start, end, errors)
    em.flow = flow
    body = []
    last = None
    for ea in range(start, end, 4):
        i = ppc.decode(prog.word(ea))
        if i is None:
            errors.append(f"{ea:08X}: undecodable {prog.word(ea):08X}")
            flow.impure.append("error")
            body.append((ea, None, [f"rt_bad_branch(ctx, {emit.hx(ea)}, 0u); return;"]))
            continue
        if ea in prog.imm_import:
            r = relocated(prog, i, ea)
            if isinstance(r, str):
                body.append((ea, i.op, [r]))
                last = i
                continue
            i = r
        try:
            lines = em.emit(i, ea)
        except NotImplementedError as e:
            errors.append(f"{ea:08X}: {e}")
            flow.impure.append("error")
            lines = [f"rt_bad_branch(ctx, {emit.hx(ea)}, 0u); return;"]
        if ea in prog.tick_rules:                  # D21: only on whole ticks at 60 fps, or with a time step
            problem = prog.check_tick_rule(ea, i)
            if problem:
                errors.append(problem)
            else:
                lines = apply_tick_rule(prog.tick_rules[ea], i, lines)
        body.append((ea, i.op, lines))
        last = i
    # __restrict: guest memory (memory_base) never overlaps the register state, so the compiler may
    # keep registers in host registers across guest stores (the shards build with
    # -fno-strict-aliasing); calls still receive ctx, so it writes back and reloads around them
    name = ("orig_" if start in prog.overrides else "") + fname(start)    # D9: f_X is src/overrides'
    out = [f"void {name}(PPCInterpreter_t* __restrict ctx)", "{"]
    out += emit_blocks(body, flow.labels)      # one cycle per instruction, charged per block (D6)
    if last is None or not is_terminator(last):
        if end in prog.entries:
            flow.callees.add(end)
            out.append(f"\t[[clang::musttail]] return {fname(end)}(ctx);   // falls through")
        else:
            errors.append(f"{start:08X}: falls off its end at {end:08X}, which is not an entry")
            flow.impure.append("error")
            out.append(f"\trt_bad_branch(ctx, {emit.hx(end)}, 0u);")
    out.append("}")
    return out, flow


def pure_functions(flows):
    """D8.2: a function is pure if it is not impure itself and calls only pure functions."""
    callers = collections.defaultdict(set)
    for a, f in flows.items():
        for c in f.callees:
            callers[c].add(a)
    impure = {a for a, f in flows.items() if f.impure or any(c not in flows for c in f.callees)}
    work = list(impure)
    while work:
        for caller in callers[work.pop()]:
            if caller not in impure:
                impure.add(caller)
                work.append(caller)
    return set(flows) - impure


def code_hash(prog, start, end, masked):
    """FNV-1a (64-bit) over the function's words as in the RPX, with words the loader rewrites
    (import and weak relocations) taken as 0. The runtime hashes guest memory the same way."""
    h = 0xCBF29CE484222325
    for ea in range(start, end, 4):
        h = ((h ^ (0 if ea in masked else prog.word(ea))) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def write_if_changed(path, text):
    if path.exists() and path.read_text() == text:
        return False
    path.write_text(text)
    return True


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        sys.exit(__doc__)
    prog = Program(args[0])
    out = pathlib.Path(args[1])
    out.mkdir(parents=True, exist_ok=True)
    per = int(args[args.index("--per-shard") + 1]) if "--per-shard" in args else 256
    global TICK_PER_INSTRUCTION
    TICK_PER_INSTRUCTION = "--tick" in args and args[args.index("--tick") + 1] == "instruction"
    em = emit.Emitter(None)
    errors = []
    header = "// Generated by tools/recomp/generate.py from the game's code: never commit.\n"
    decls = [header, "#pragma once", '#include "ppc_ops.h"', "",
             "void rt_import(PPCInterpreter_t* ctx, uint32 importId);   // D4: HLE import call",
             "uint32 rt_import_data(uint32 importId);                    // D4: data import address",
             "void rt_call_ctr(PPCInterpreter_t* ctx);                   // D5: bctrl",
             "void rt_jump_ctr(PPCInterpreter_t* ctx);                   // D5: bctr (tail)",
             "void rt_bad_branch(PPCInterpreter_t* ctx, uint32 ea, uint32 target);", ""]
    decls += [f"void {fname(a)}(PPCInterpreter_t* ctx);" for a, _, _ in prog.funcs]
    decls += ["", "// D9: the generated bodies of the functions src/overrides replaces"]
    decls += [f"void orig_{fname(a)}(PPCInterpreter_t* ctx);" for a in sorted(prog.overrides)]
    write_if_changed(out / "funcs.h", "\n".join(decls) + "\n")
    changed = 0
    flows = {}
    shards = [prog.funcs[k:k + per] for k in range(0, len(prog.funcs), per)]
    for n, shard in enumerate(shards):
        lines = [header, '#include "funcs.h"', ""]
        for a, e, name in shard:
            nm = prog.names.get(a)
            if nm:
                lines.append(f"// {nm}")
            body, flows[a] = generate_function(prog, em, a, e, errors)
            lines += body
            lines.append("")
        changed += write_if_changed(out / f"shard_{n:03d}.cpp", "\n".join(lines))
    pure = pure_functions(flows)
    synthetic = {a for a, _, _ in prog.synthetic}
    # words the loader rewrites: calls/branches to imports and weak symbols, data-import immediates
    sites = sorted([(ea, prog.import_ids[t], REL24, 0) for ea, t in prog.rel24.items() if t in prog.imports]
                   + [(ea, 0xFFFF, 0, 0) for ea, t in prog.rel24.items() if t == 0]
                   + [(ea, prog.import_ids[t], typ, a) for ea, (typ, t, a) in prog.imm_import.items()])
    masked = {s[0] for s in sites}
    table = [header, '#include "funcs.h"', '#include "recomp_tables.h"', "",
             "extern const uint32_t g_recompTablesVersion = kRecompTablesVersion;",
             "extern const RecompFunc g_funcTable[] = {"]
    index = {a: n for n, (a, _, _) in enumerate(prog.funcs)}
    callees = []
    for a, e, _ in prog.funcs:
        flags = (1 if a in pure else 0) | (2 if a in synthetic else 0)
        edges = sorted(index[c] for c in flows[a].callees)
        table.append(f"\t{{0x{a:08X}u, 0x{e:08X}u, {fname(a)}, {flags}u, 0x{code_hash(prog, a, e, masked):016X}ull, "
                     f"{len(callees)}u, {len(edges)}u}},")
        callees += edges
    table += ["};", f"extern const size_t g_funcCount = {len(prog.funcs)};", "",
              "extern const uint32_t g_callees[] = {"]
    table += ["\t" + ", ".join(f"{c}u" for c in callees[k:k + 16]) + "," for k in range(0, len(callees), 16)]
    table += ["};"]
    write_if_changed(out / "func_table.cpp", "\n".join(table) + "\n")
    imps = [header, '#include "recomp_tables.h"', "", "extern const RecompImport g_imports[] = {"]
    imps += [f'\t{{0x{a:08X}u, "{prog.imports[a][0]}", "{prog.imports[a][1]}", {int(prog.import_is_data[a])}u}},'
             for a in sorted(prog.imports)]
    imps += ["};", f"extern const size_t g_importCount = {len(prog.imports)};", "",
             "extern const RecompImportSite g_importSites[] = {"]
    imps += [f"\t{{0x{ea:08X}u, {i}u, {k}u, {a}}}," for ea, i, k, a in sites]
    imps += ["};", f"extern const size_t g_importSiteCount = {len(sites)};", ""]
    stores = collections.Counter()
    for a, e, _ in prog.funcs:
        if a not in synthetic:
            for ea in range(a, e, 4):
                i = ppc.decode(prog.word(ea))
                if i is not None and i.op in STORES:
                    stores[i.op] += 1
    imps += ["extern const RecompStoreCount g_storeCensus[] = {"]
    imps += [f'\t{{"{m}", {stores[m]}u}},' for m in sorted(STORES)]
    imps += ["};", f"extern const size_t g_storeCensusCount = {len(STORES)};"]
    write_if_changed(out / "imports.cpp", "\n".join(imps) + "\n")
    reasons = collections.Counter(r for f in flows.values() for r in set(f.impure))
    print(f"pure: {len(pure)} of {len(flows)} functions; impure by themselves: {dict(reasons)}; "
          f"import/weak sites: {len(sites)}; stores: {sum(stores.values())}")
    print(f"{len(prog.funcs)} functions ({len(prog.synthetic)} synthesised GHS helper entries, {len(prog.overrides)} overridden, "
          f"{len(prog.tick_rules)} tick rules) "
          f"in {len(shards)} shards ({changed} rewritten), {len(prog.imports)} imports, {len(errors)} errors -> {out}")
    kinds = collections.Counter(e.split(": ", 1)[1].split(" ")[0] for e in errors)
    for e in errors[:200]:
        print("  " + e)
    if kinds:
        print("error kinds:", dict(kinds))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
