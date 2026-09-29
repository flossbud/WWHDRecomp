"""Whole-program generator (docs/recompiler-design.md D1, D4, D5, D7; milestone M2).

Usage:
    python3 tools/recomp/generate.py orig/0005000010143500_v0/code/cking.rpx OUT_DIR [--per-shard 256]

Writes OUT_DIR/shard_NNN.cpp (one C++ function per guest function in functions.csv), funcs.h
(declarations), func_table.cpp ({guest address, host function}) and imports.cpp (the import
table the runtime binds to Cemu's HLE handlers). Output is generated from game code: it goes to
build/ and is never committed. A shard is rewritten only when its content changes.

Control flow (D1): branches inside a function become gotos; a branch to another function's
entry is a musttail call; bl is a call; blr is return; a bctr listed in jump_tables.csv is a
switch on CTR; other bctr/bctrl go through the runtime's function table. A function whose last
instruction can fall through tail-calls the next function, which is also how the GHS save/restore
helpers (D7) chain from entry to entry. Branches to imports (REL24 relocations into .fimport_*)
call the HLE import; immediates relocated against data imports (.dimport_*) are read from the
runtime. Anything the generator cannot place is an error, listed at the end; exit status 1.
"""
import bisect
import collections
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


class Program:
    def __init__(self, rpx_path):
        rpx = Rpx(rpx_path)
        self.sections = [(rpx.name(i), s[3], rpx.size(i)) for i, s in enumerate(rpx.sh) if s[3]]
        # code: .text, plus .syscall (an 8-byte "nop; blr" stub that some code calls)
        self.code = [(rpx.sh[i][3], rpx.data(i)) for i in range(len(rpx.sh)) if rpx.name(i) in (".text", ".syscall")]
        # imports: symbols defined in .fimport_LIB (functions) and .dimport_LIB (data)
        self.imports = {}
        for i, s in enumerate(rpx.sh):
            if s[1] != 2:                                  # SYMTAB
                continue
            syms, strtab = rpx.data(i), rpx.data(s[6])
            for k in range(0, len(syms), 16):
                name_off, value, _size, _info, _other, shndx = struct.unpack_from(">IIIBBH", syms, k)
                if 0 < shndx < len(rpx.sh) and rpx.name(shndx).startswith((".fimport_", ".dimport_")):
                    name = strtab[name_off:strtab.index(b"\0", name_off)].decode()
                    self.imports[value] = (rpx.name(shndx).split("_", 1)[1], name)
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
        self.rel24, self.imm_import = {}, {}
        for sec, off, typ, tgt in rpx.relocations():
            if sec != ".text":
                continue
            if typ == REL24:
                self.rel24[off] = tgt
            elif typ in (ADDR16_LO, ADDR16_HI, ADDR16_HA) and tgt in self.imports:
                self.imm_import[off & ~3] = (typ, tgt)
        self.jump_tables = {}
        with open(CONFIG / "jump_tables.csv") as f:
            for r in csv.DictReader(f):
                self.jump_tables[int(r["bctr"], 16)] = [int(t, 16) for t in r["targets"].split()]
        self.names = {}
        sym = CONFIG / "symbols.csv"
        if sym.exists():
            with open(sym) as f:
                for r in csv.DictReader(f):
                    try:
                        self.names[int(r["address"], 16)] = r["name"]
                    except (KeyError, ValueError):
                        pass
        self.synthetic = self.helper_entries()
        if self.synthetic:
            self.funcs = sorted(self.funcs + self.synthetic)
            self.entries |= {a for a, _, _ in self.synthetic}

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

    def section_of(self, a):
        for name, base, size in self.sections:
            if base <= a < base + size:
                return name
        return "?"

    def in_text(self, a):
        return any(base <= a < base + len(data) for base, data in self.code)

    def word(self, ea):
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
            body = [f"rt_import(ctx, {p.import_ids[target]}); // {'.'.join(p.imports[target])}"]
            if link:
                body.insert(0, f"ctx->spr.LR = {emit.hx(ea + 4)};")
            else:
                body.append("return;")                    # tail call to the import
            return self._wrap(cond, body)
        if target == 0:
            return self._wrap(cond, [f"rt_bad_branch(ctx, {emit.hx(ea)}, 0u); return;"])
        if link:
            if target == ea + 4 and target < self.end:      # "bl $+4": reads the PC into LR
                return self._wrap(cond, [f"ctx->spr.LR = {emit.hx(ea + 4)};"])
            if target not in p.entries:
                self.errors.append(f"{ea:08X}: call to {target:08X}, not a function entry")
                return self._wrap(cond, [f"rt_bad_branch(ctx, {emit.hx(ea)}, {emit.hx(target)}); return;"])
            return self._wrap(cond, [f"ctx->spr.LR = {emit.hx(ea + 4)};", f"{fname(target)}(ctx);"])
        if self.start <= target < self.end:
            self.labels.add(target)
            return [f"goto L_{target:08X};"] if cond is None else [f"if ({cond}) goto L_{target:08X};"]
        if target in p.entries:
            return self._wrap(cond, [f"[[clang::musttail]] return {fname(target)}(ctx);"])
        self.errors.append(f"{ea:08X}: branch to {target:08X}, neither in the function nor an entry")
        return self._wrap(cond, [f"rt_bad_branch(ctx, {emit.hx(ea)}, {emit.hx(target)}); return;"])

    def branch_lr(self, ea, cond, link):
        if link:
            self.errors.append(f"{ea:08X}: bclrl")
        return ["return;"] if cond is None else [f"if ({cond}) return;"]

    def branch_ctr(self, ea, cond, link):
        if link:
            return self._wrap(cond, [f"ctx->spr.LR = {emit.hx(ea + 4)};", "rt_call_ctr(ctx);"])
        table = self.prog.jump_tables.get(ea)
        if table is None:
            return self._wrap(cond, ["[[clang::musttail]] return rt_jump_ctr(ctx);"])
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
    typ, tgt = prog.imm_import[ea]
    expr = f"rt_import_data({prog.import_ids[tgt]})"
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


def generate_function(prog, em, start, end, errors):
    flow = FunctionFlow(prog, start, end, errors)
    em.flow = flow
    body = []
    last = None
    for ea in range(start, end, 4):
        i = ppc.decode(prog.word(ea))
        if i is None:
            errors.append(f"{ea:08X}: undecodable {prog.word(ea):08X}")
            body.append((ea, [f"rt_bad_branch(ctx, {emit.hx(ea)}, 0u); return;"]))
            continue
        if ea in prog.imm_import:
            r = relocated(prog, i, ea)
            if isinstance(r, str):
                body.append((ea, [r]))
                last = i
                continue
            i = r
        try:
            lines = em.emit(i, ea)
        except NotImplementedError as e:
            errors.append(f"{ea:08X}: {e}")
            lines = [f"rt_bad_branch(ctx, {emit.hx(ea)}, 0u); return;"]
        body.append((ea, lines))
        last = i
    out = [f"void {fname(start)}(PPCInterpreter_t* ctx)", "{"]
    for ea, lines in body:
        if ea in flow.labels:
            out.append(f"L_{ea:08X}:;")
        out += ["\t" + l for l in lines]
    if last is None or not is_terminator(last):
        if end in prog.entries:
            out.append(f"\t[[clang::musttail]] return {fname(end)}(ctx);   // falls through")
        else:
            errors.append(f"{start:08X}: falls off its end at {end:08X}, which is not an entry")
            out.append(f"\trt_bad_branch(ctx, {emit.hx(end)}, 0u);")
    out.append("}")
    return out


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
    write_if_changed(out / "funcs.h", "\n".join(decls) + "\n")
    changed = 0
    shards = [prog.funcs[k:k + per] for k in range(0, len(prog.funcs), per)]
    for n, shard in enumerate(shards):
        lines = [header, '#include "funcs.h"', ""]
        for a, e, name in shard:
            nm = prog.names.get(a)
            if nm:
                lines.append(f"// {nm}")
            lines += generate_function(prog, em, a, e, errors)
            lines.append("")
        changed += write_if_changed(out / f"shard_{n:03d}.cpp", "\n".join(lines))
    table = [header, '#include "funcs.h"', "", "struct FuncEntry { uint32 address; void (*fn)(PPCInterpreter_t*); };",
             f"extern const FuncEntry g_funcTable[] = {{"]
    table += [f"\t{{0x{a:08X}u, {fname(a)}}}," for a, _, _ in prog.funcs]
    table += ["};", f"extern const size_t g_funcCount = {len(prog.funcs)};"]
    write_if_changed(out / "func_table.cpp", "\n".join(table) + "\n")
    imps = [header, "#include <cstdint>", "#include <cstddef>", "",
            "struct ImportEntry { uint32_t stub; const char* lib; const char* name; };",
            "extern const ImportEntry g_imports[] = {"]
    imps += [f'\t{{0x{a:08X}u, "{prog.imports[a][0]}", "{prog.imports[a][1]}"}},' for a in sorted(prog.imports)]
    imps += ["};", f"extern const size_t g_importCount = {len(prog.imports)};"]
    write_if_changed(out / "imports.cpp", "\n".join(imps) + "\n")
    print(f"{len(prog.funcs)} functions ({len(prog.synthetic)} synthesised GHS helper entries) in {len(shards)} shards ({changed} rewritten), "
          f"{len(prog.imports)} imports, {len(errors)} errors -> {out}")
    kinds = collections.Counter(e.split(": ", 1)[1].split(" ")[0] for e in errors)
    for e in errors[:200]:
        print("  " + e)
    if kinds:
        print("error kinds:", dict(kinds))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
