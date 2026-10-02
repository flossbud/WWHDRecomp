"""C++ emission for single Espresso instructions (docs/recompiler-design.md D1-D3).

    lines = Emitter(flow).emit(insn, ea)

Semantics mirror Cemu's interpreter (the M1 oracle, see tools/recomp/fuzz/), statement for
statement where the interpreter has quirks (FP rounding through float, roundTo25BitAccuracy,
flushDenormalToZero in ps_mul/ps_madd, divw by zero, ...). Helpers live in runtime/ppc_ops.h.

Control flow is delegated to a `flow` object so the same instruction semantics serve both the
fuzzer (StepFlow: every branch just sets instructionPointer and returns) and the function-level
generator (M2: gotos, tail calls, dispatch).
"""
import ppc


def mask(mb, me):
    m_mb = 0xFFFFFFFF >> mb
    m_me = (0xFFFFFFFF << (31 - me)) & 0xFFFFFFFF
    return (m_mb & m_me) if mb <= me else (m_mb | m_me)


def ra0(i):
    """(rA|0): the base register, or literal 0 when the rA field is 0."""
    return f"GPR({i.rA})" if i.rA else "0u"


def hx(v):
    """Immediate as C: an int, or a C expression string (runtime-relocated import addresses)."""
    return f"(uint32)({v})" if isinstance(v, str) else f"0x{v & 0xFFFFFFFF:X}u"


class StepFlow:
    """Single-instruction control flow: the instruction leaves the next address in
    instructionPointer. Used by the fuzzer, where each test runs exactly one instruction.

    A flow provides fallthrough(ea) and the three branch kinds; `cond` is a C condition or None,
    `link` whether LR gets the return address:
        branch_to(ea, target, cond, link)   b / bc / bl / bcl to a known address
        branch_lr(ea, cond, link)           bclr (return)
        branch_ctr(ea, cond, link)          bcctr (switch, indirect jump or call)"""

    def fallthrough(self, ea):
        return [f"ctx->instructionPointer = {hx(ea + 4)};"]

    def _jump(self, ea, target_expr, cond, link):
        body = [f"uint32 t_ = {target_expr};"]
        if link:
            body.append(f"ctx->spr.LR = {hx(ea + 4)};")
        body += ["ctx->instructionPointer = t_;", "return;"]
        if cond is None:
            return ["{"] + ["\t" + b for b in body] + ["}"]
        return [f"if ({cond}) {{"] + ["\t" + b for b in body] + ["}"] + self.fallthrough(ea)

    def branch_to(self, ea, target, cond, link):
        return self._jump(ea, hx(target), cond, link)

    def branch_lr(self, ea, cond, link):
        return self._jump(ea, "ctx->spr.LR & ~3u", cond, link)

    def branch_ctr(self, ea, cond, link):
        return self._jump(ea, "ctx->spr.CTR & ~3u", cond, link)


class Emitter:
    def __init__(self, flow):
        self.flow = flow

    BRANCHES = {"b", "bc", "bclr", "bcctr"}   # these hand the fallthrough to `flow` themselves

    def emit(self, i, ea):
        if i.op in self.LOADSTORE:
            out = self._loadstore(i, ea, *self.LOADSTORE[i.op])
        else:
            fn = getattr(self, "op_" + i.op.replace(".", "_"), None)
            if fn is None:
                raise NotImplementedError(f"{i.op} at {ea:08X}")
            out = fn(i, ea)
        return out if i.op in self.BRANCHES else out + self.flow.fallthrough(ea)

    # helpers ------------------------------------------------------------------------------------
    @staticmethod
    def rc(i, reg):
        return [f"cr_record(ctx, GPR({reg}));"] if i.rc else []

    # integer arithmetic --------------------------------------------------------------------------
    def op_addi(self, i, ea):
        return [f"GPR({i.rD}) = {ra0(i)} + {hx(i.simm)};"]

    def op_addis(self, i, ea):
        return [f"GPR({i.rD}) = {ra0(i)} + {hx(i.simm << 16)};"]

    def op_addic(self, i, ea, record=False):
        out = [f"{{ uint32 a = GPR({i.rA}); uint32 r = a + {hx(i.simm)}; GPR({i.rD}) = r; ctx->xer_ca = r < a; }}"]
        return out + ([f"cr_record(ctx, GPR({i.rD}));"] if record else [])

    def op_addic_(self, i, ea):
        return self.op_addic(i, ea, True)

    def op_subfic(self, i, ea):
        return [f"{{ uint32 na = ~GPR({i.rA}); uint64 s = (uint64)na + {hx(i.simm)} + 1; "
                f"GPR({i.rD}) = (uint32)s; ctx->xer_ca = (s >> 32) != 0; }}"]

    def op_mulli(self, i, ea):
        return [f"GPR({i.rD}) = GPR({i.rA}) * {hx(i.simm)};"]

    def _xo(self, i, expr_lines):
        """XO-form: expr_lines compute r (uint32) from a = rA, b = rB. WWHD has no OE=1 forms."""
        if i.oe:
            raise NotImplementedError(f"{i.op}o (OE=1)")
        out = [f"{{ uint32 a = GPR({i.rA}); uint32 b = GPR({i.rB}); (void)b;"] + ["\t" + l for l in expr_lines]
        return out + [f"\tGPR({i.rD}) = r; }}"] + self.rc(i, i.rD)

    def op_add(self, i, ea):
        return self._xo(i, ["uint32 r = a + b;"])

    def op_subf(self, i, ea):
        return self._xo(i, ["uint32 r = ~a + b + 1;"])

    def op_addc(self, i, ea):
        return self._xo(i, ["uint32 r = a + b; ctx->xer_ca = r < a;"])

    def op_subfc(self, i, ea):
        return self._xo(i, ["uint64 s = (uint64)(uint32)~a + b + 1; uint32 r = (uint32)s; ctx->xer_ca = (s >> 32) != 0;"])

    def op_adde(self, i, ea):
        return self._xo(i, ["uint64 s = (uint64)a + b + ctx->xer_ca; uint32 r = (uint32)s; ctx->xer_ca = (s >> 32) != 0;"])

    def op_subfe(self, i, ea):
        return self._xo(i, ["uint64 s = (uint64)(uint32)~a + b + ctx->xer_ca; uint32 r = (uint32)s; ctx->xer_ca = (s >> 32) != 0;"])

    def op_addze(self, i, ea):
        return self._xo(i, ["uint32 r = a + ctx->xer_ca; ctx->xer_ca = (a == 0xFFFFFFFFu) && ctx->xer_ca;"])

    def op_subfze(self, i, ea):
        return self._xo(i, ["uint32 r = ~a + ctx->xer_ca; ctx->xer_ca = (a == 0) && ctx->xer_ca;"])

    def op_addme(self, i, ea):
        return self._xo(i, ["uint32 r = a + ctx->xer_ca + 0xFFFFFFFFu; ctx->xer_ca = (a != 0) || ctx->xer_ca;"])

    def op_subfme(self, i, ea):
        return self._xo(i, ["uint64 s = (uint64)(uint32)~a + 0xFFFFFFFFu + ctx->xer_ca; uint32 r = (uint32)s; ctx->xer_ca = (s >> 32) != 0;"])

    def op_neg(self, i, ea):
        return self._xo(i, ["uint32 r = 0u - a;"])   # not -(sint32)a: UB for INT_MIN, and clang exploits it

    def op_mullw(self, i, ea):
        return self._xo(i, ["uint32 r = a * b;"])

    def op_mulhw(self, i, ea):
        return self._xo(i, ["uint32 r = (uint32)(((sint64)(sint32)a * (sint64)(sint32)b) >> 32);"])

    def op_mulhwu(self, i, ea):
        return self._xo(i, ["uint32 r = (uint32)(((uint64)a * (uint64)b) >> 32);"])

    def op_divw(self, i, ea):
        return self._xo(i, ["uint32 r;",
                            "if (b == 0) r = (sint32)a < 0 ? 0xFFFFFFFFu : 0;",
                            "else if (a == 0x80000000u && b == 0xFFFFFFFFu) r = 0xFFFFFFFFu;",
                            "else r = (uint32)((sint32)a / (sint32)b);"])

    def op_divwu(self, i, ea):
        return self._xo(i, ["uint32 r;",
                            "if (b == 0) r = 0;",
                            "else if (a == 0x80000000u && b == 0xFFFFFFFFu) r = 0;",
                            "else r = a / b;"])

    # compare --------------------------------------------------------------------------------------
    def op_cmp(self, i, ea):
        return [f"cr_compare<sint32>(ctx, {i.crfD}, (sint32)GPR({i.rA}), (sint32)GPR({i.rB}));"]

    def op_cmpl(self, i, ea):
        return [f"cr_compare<uint32>(ctx, {i.crfD}, GPR({i.rA}), GPR({i.rB}));"]

    def op_cmpi(self, i, ea):
        return [f"cr_compare<sint32>(ctx, {i.crfD}, (sint32)GPR({i.rA}), {i.simm});"]

    def op_cmpli(self, i, ea):
        return [f"cr_compare<uint32>(ctx, {i.crfD}, GPR({i.rA}), {hx(i.uimm)});"]

    # logical ------------------------------------------------------------------------------------
    def _logi(self, i, expr, record=False):
        return [f"GPR({i.rA}) = {expr};"] + ([f"cr_record(ctx, GPR({i.rA}));"] if record else [])

    def op_ori(self, i, ea): return self._logi(i, f"GPR({i.rS}) | {hx(i.uimm)}")
    def op_oris(self, i, ea): return self._logi(i, f"GPR({i.rS}) | {hx(i.uimm << 16)}")
    def op_xori(self, i, ea): return self._logi(i, f"GPR({i.rS}) ^ {hx(i.uimm)}")
    def op_xoris(self, i, ea): return self._logi(i, f"GPR({i.rS}) ^ {hx(i.uimm << 16)}")
    def op_andi_(self, i, ea): return self._logi(i, f"GPR({i.rS}) & {hx(i.uimm)}", True)
    def op_andis_(self, i, ea): return self._logi(i, f"GPR({i.rS}) & {hx(i.uimm << 16)}", True)

    def _logx(self, i, expr):
        return [f"GPR({i.rA}) = {expr};"] + self.rc(i, i.rA)

    def op_and(self, i, ea): return self._logx(i, f"GPR({i.rT}) & GPR({i.rB})")
    def op_andc(self, i, ea): return self._logx(i, f"GPR({i.rT}) & ~GPR({i.rB})")
    def op_or(self, i, ea): return self._logx(i, f"GPR({i.rT}) | GPR({i.rB})")
    def op_orc(self, i, ea): return self._logx(i, f"GPR({i.rT}) | ~GPR({i.rB})")
    def op_xor(self, i, ea): return self._logx(i, f"GPR({i.rT}) ^ GPR({i.rB})")
    def op_nor(self, i, ea): return self._logx(i, f"~(GPR({i.rT}) | GPR({i.rB}))")
    def op_nand(self, i, ea): return self._logx(i, f"~(GPR({i.rT}) & GPR({i.rB}))")
    def op_eqv(self, i, ea): return self._logx(i, f"~(GPR({i.rT}) ^ GPR({i.rB}))")
    def op_extsb(self, i, ea): return self._logx(i, f"(uint32)(sint32)(sint8)GPR({i.rT})")
    def op_extsh(self, i, ea): return self._logx(i, f"(uint32)(sint32)(sint16)GPR({i.rT})")
    def op_cntlzw(self, i, ea): return self._logx(i, f"(GPR({i.rT}) ? (uint32)__builtin_clz(GPR({i.rT})) : 32u)")

    # shifts and rotates ---------------------------------------------------------------------------
    def op_slw(self, i, ea):
        return self._logx(i, f"((GPR({i.rB}) & 0x3F) > 31 ? 0u : GPR({i.rT}) << (GPR({i.rB}) & 0x3F))")

    def op_srw(self, i, ea):
        return self._logx(i, f"((GPR({i.rB}) & 0x3F) > 31 ? 0u : GPR({i.rT}) >> (GPR({i.rB}) & 0x3F))")

    def op_sraw(self, i, ea):
        return [f"{{ uint32 s = GPR({i.rT}); uint32 sh = GPR({i.rB}) & 0x3F;",
                "\tif (sh > 31) { ctx->xer_ca = s >> 31; s = (uint32)((sint32)s >> 31); }",
                "\telse { ctx->xer_ca = (s >> 31) & ((s & ~(0xFFFFFFFFu << sh)) != 0); s = (uint32)((sint32)s >> sh); }",
                f"\tGPR({i.rA}) = s; }}"] + self.rc(i, i.rA)

    def op_srawi(self, i, ea):
        sh = i.rB
        lost = hx((1 << sh) - 1) if sh else "0u"
        return [f"{{ uint32 s = GPR({i.rT}); ctx->xer_ca = (s >> 31) & ((s & {lost}) != 0);",
                f"\tGPR({i.rA}) = (uint32)((sint32)s >> {sh}); }}"] + self.rc(i, i.rA)

    def op_rlwinm(self, i, ea):
        return [f"GPR({i.rA}) = std::rotl<uint32>(GPR({i.rS}), {i.sh}) & {hx(mask(i.mb, i.me))};"] + self.rc(i, i.rA)

    def op_rlwnm(self, i, ea):
        return [f"GPR({i.rA}) = std::rotl<uint32>(GPR({i.rS}), GPR({i.rB}) & 0x1F) & {hx(mask(i.mb, i.me))};"] + self.rc(i, i.rA)

    def op_rlwimi(self, i, ea):
        m = mask(i.mb, i.me)
        return [f"GPR({i.rA}) = (std::rotl<uint32>(GPR({i.rS}), {i.sh}) & {hx(m)}) | (GPR({i.rA}) & {hx(~m)});"] + self.rc(i, i.rA)

    # loads and stores --------------------------------------------------------------------------
    # Update forms write rA after the access, as Cemu does; the game never uses rA=0 or rA=rD there.
    def _ea_d(self, i):
        return f"{ra0(i)} + {hx(i.d)}"

    def _ea_x(self, i):
        return f"{ra0(i)} + GPR({i.rB})"

    LOADS = {"lwz": "rd32(ea)", "lbz": "rd8(ea)", "lhz": "rd16(ea)", "lha": "(uint32)(sint32)(sint16)rd16(ea)"}
    # a store names its instruction for the store journal (RT_STORE: the census and the 60 fps tools)
    STORES = {"stw": "wr32(ea, GPR({0}), {1})", "stb": "wr8(ea, (uint8)GPR({0}), {1})", "sth": "wr16(ea, (uint16)GPR({0}), {1})"}
    LOADSTORE = {}          # mnemonic -> (template, indexed, update)
    for _base, _t in list(LOADS.items()) + list(STORES.items()):
        for _sfx, _x, _u in (("", False, False), ("u", False, True), ("x", True, False), ("ux", True, True)):
            LOADSTORE[_base + _sfx] = (_t, _x, _u)
    LOADSTORE.update({"lwbrx": ("__builtin_bswap32(rd32(ea))", True, False),
                      "lhbrx": ("(uint32)__builtin_bswap16(rd16(ea))", True, False),
                      "stwbrx": ("wr32(ea, __builtin_bswap32(GPR({0})), {1})", True, False),
                      "sthbrx": ("wr16(ea, __builtin_bswap16((uint16)GPR({0})), {1})", True, False)})

    def _loadstore(self, i, pc, template, indexed, update):
        ea = self._ea_x(i) if indexed else self._ea_d(i)
        access = template.format(i.rT, hx(pc)) if template.startswith("wr") else f"GPR({i.rT}) = {template}"
        return [f"{{ uint32 ea = {ea}; {access};" + (f" GPR({i.rA}) = ea;" if update else "") + " }"]

    def op_lmw(self, i, ea):
        return [f"{{ uint32 ea = {self._ea_d(i)}; for (int r = {i.rT}; r < 32; r++, ea += 4) GPR(r) = rd32(ea); }}"]

    def op_stmw(self, i, ea):
        return [f"{{ uint32 ea = {self._ea_d(i)}; for (int r = {i.rT}; r < 32; r++, ea += 4) wr32(ea, GPR(r), {hx(ea)}); }}"]

    def op_lswi(self, i, ea):
        nb = i.nb or 32
        out = [f"{{ uint32 ea = {ra0(i)};"]
        r = i.rT
        for k in range(0, nb, 4):
            n = min(4, nb - k)
            parts = [f"((uint32)rd8(ea + {k + j}) << {24 - 8 * j})" for j in range(n)]
            out.append(f"\tGPR({r}) = {' | '.join(parts)};")
            r = (r + 1) % 32
        return out + ["}"]

    def op_stswi(self, i, ea):
        nb = i.nb or 32
        out = [f"{{ uint32 ea = {ra0(i)};"]
        r = i.rT
        for k in range(0, nb, 4):
            for j in range(min(4, nb - k)):
                out.append(f"\twr8(ea + {k + j}, (uint8)(GPR({r}) >> {24 - 8 * j}), {hx(ea)});")
            r = (r + 1) % 32
        return out + ["}"]

    def op_lwarx(self, i, ea):
        return [f"{{ uint32 ea = {self._ea_x(i)}; uint32 v = rd32(ea); GPR({i.rT}) = v;",
                "\tctx->reservedMemAddr = ea; ctx->reservedMemValue = v; }"]

    def op_stwcx_(self, i, ea):
        # Cemu: succeed iff the reservation address matches and memory still holds the reserved
        # value (compare-and-swap); either way CR0 = 00 EQ SO, and a matching attempt clears it.
        return [f"{{ uint32 ea = {self._ea_x(i)}; bool ok = false;",
                "\tif (ctx->reservedMemAddr == ea) {",
                f"\t\tif (g_rtJournalOn && rd32(ea) == ctx->reservedMemValue) rt_journal_store(ea, 4, {hx(ea)});   // it will store",
                "\t\tstd::atomic_ref<uint32> w(*(uint32*)(memory_base + ea));",
                "\t\tuint32 expect = __builtin_bswap32(ctx->reservedMemValue);",
                f"\t\tok = w.compare_exchange_strong(expect, __builtin_bswap32(GPR({i.rT})));",
                "\t\tctx->cr[CR_BIT_SO] = ctx->xer_so;",
                "\t\tctx->reservedMemAddr = 0; ctx->reservedMemValue = 0;",
                "\t}",
                "\tctx->cr[CR_BIT_LT] = 0; ctx->cr[CR_BIT_GT] = 0; ctx->cr[CR_BIT_EQ] = ok; }"]

    def op_dcbz(self, i, ea):
        return [f"zero_line({self._ea_x(i)}, {hx(ea)});"]

    def op_dcbf(self, i, ea):
        return [f"rt_dcache_flush({self._ea_x(i)});"]

    op_dcbst = op_dcbf

    def op_dcbt(self, i, ea): return []
    def op_dcbtst(self, i, ea): return []
    def op_dcbi(self, i, ea): return []
    def op_dcbz_l(self, i, ea): return []
    def op_isync(self, i, ea): return []
    def op_sync(self, i, ea): return []
    def op_eieio(self, i, ea): return []

    # floating-point loads/stores ---------------------------------------------------------------
    def _lfs(self, i, ea_expr, update):
        return [f"{{ uint32 ea = {ea_expr}; uint64 v = ConvertToDoubleNoFTZ(rd32(ea)); "
                f"FPR({i.rT}).fp0int = v; FPR({i.rT}).fp1int = v;" + (f" GPR({i.rA}) = ea;" if update else "") + " }"]

    def _stfs(self, i, ea_expr, update, pc):
        return [f"{{ uint32 ea = {ea_expr}; wr32(ea, ConvertToSingleNoFTZ(FPR({i.rT}).fp0int), {hx(pc)});"
                + (f" GPR({i.rA}) = ea;" if update else "") + " }"]

    def _lfd(self, i, ea_expr, update):
        return [f"{{ uint32 ea = {ea_expr}; FPR({i.rT}).fp0int = rd64(ea);" + (f" GPR({i.rA}) = ea;" if update else "") + " }"]

    def _stfd(self, i, ea_expr, update, pc):
        return [f"{{ uint32 ea = {ea_expr}; wr64(ea, FPR({i.rT}).fp0int, {hx(pc)});" + (f" GPR({i.rA}) = ea;" if update else "") + " }"]

    def op_lfs(self, i, ea): return self._lfs(i, self._ea_d(i), False)
    def op_lfsu(self, i, ea): return self._lfs(i, self._ea_d(i), True)
    def op_lfsx(self, i, ea): return self._lfs(i, self._ea_x(i), False)
    def op_lfsux(self, i, ea): return self._lfs(i, self._ea_x(i), True)
    def op_stfs(self, i, ea): return self._stfs(i, self._ea_d(i), False, ea)
    def op_stfsu(self, i, ea): return self._stfs(i, self._ea_d(i), True, ea)
    def op_stfsx(self, i, ea): return self._stfs(i, self._ea_x(i), False, ea)
    def op_stfsux(self, i, ea): return self._stfs(i, self._ea_x(i), True, ea)
    def op_lfd(self, i, ea): return self._lfd(i, self._ea_d(i), False)
    def op_lfdu(self, i, ea): return self._lfd(i, self._ea_d(i), True)
    def op_lfdx(self, i, ea): return self._lfd(i, self._ea_x(i), False)
    def op_lfdux(self, i, ea): return self._lfd(i, self._ea_x(i), True)
    def op_stfd(self, i, ea): return self._stfd(i, self._ea_d(i), False, ea)
    def op_stfdu(self, i, ea): return self._stfd(i, self._ea_d(i), True, ea)
    def op_stfdx(self, i, ea): return self._stfd(i, self._ea_x(i), False, ea)
    def op_stfdux(self, i, ea): return self._stfd(i, self._ea_x(i), True, ea)

    def op_stfiwx(self, i, ea):
        return [f"wr32({self._ea_x(i)}, (uint32)FPR({i.rT}).fp0int, {hx(ea)});"]

    def _psq(self, i, load, ea_expr, update, pc=None):
        fn = "psq_load" if load else "psq_store"
        tail = f", {hx(pc)}" if not load else ""
        return [f"{{ uint32 ea = {ea_expr}; {fn}(ctx, {i.frT}, ea, {i.I}, {'true' if i.W else 'false'}{tail});"
                + (f" GPR({i.rA}) = ea;" if update and i.rA else "") + " }"]

    def op_psq_l(self, i, ea): return self._psq(i, True, f"{ra0(i)} + {hx(i.d)}", False)
    def op_psq_lu(self, i, ea): return self._psq(i, True, f"{ra0(i)} + {hx(i.d)}", True)
    def op_psq_st(self, i, ea): return self._psq(i, False, f"{ra0(i)} + {hx(i.d)}", False, ea)
    def op_psq_stu(self, i, ea): return self._psq(i, False, f"{ra0(i)} + {hx(i.d)}", True, ea)
    def op_psq_lx(self, i, ea): return self._psq(i, True, self._ea_x(i), False)
    def op_psq_stx(self, i, ea): return self._psq(i, False, self._ea_x(i), False, ea)
    def op_psq_lux(self, i, ea): return self._psq(i, True, self._ea_x(i), True)
    def op_psq_stux(self, i, ea): return self._psq(i, False, self._ea_x(i), True, ea)

    # scalar floating point (PPCInterpreterFPU.cpp; PSE is always on, so singles copy to ps1) --
    @staticmethod
    def _f(n): return f"FPR({n}).fp0"

    def _single(self, i, expr):
        return [f"FPR({i.frD}).fp0 = (float)({expr}); FPR({i.frD}).fp1 = FPR({i.frD}).fp0;"]

    def op_fmr(self, i, ea): return [f"FPR({i.frD}).fpr = FPR({i.frB}).fpr;"]
    def op_fneg(self, i, ea): return [f"FPR({i.frD}).guint = FPR({i.frB}).guint ^ (1ULL << 63);"]
    def op_fabs(self, i, ea): return [f"FPR({i.frD}).guint = FPR({i.frB}).guint & ~(1ULL << 63);"]
    def op_fnabs(self, i, ea): return [f"FPR({i.frD}).guint = FPR({i.frB}).guint | (1ULL << 63);"]
    def op_fadd(self, i, ea): return [f"FPR({i.frD}).fpr = FPR({i.frA}).fpr + FPR({i.frB}).fpr;"]
    def op_fsub(self, i, ea): return [f"FPR({i.frD}).fpr = FPR({i.frA}).fpr - FPR({i.frB}).fpr;"]
    def op_fmul(self, i, ea): return [f"FPR({i.frD}).fpr = FPR({i.frA}).fpr * FPR({i.frC}).fpr;"]
    def op_fdiv(self, i, ea): return [f"FPR({i.frD}).fpr = FPR({i.frA}).fpr / FPR({i.frB}).fpr;"]
    def op_fmadd(self, i, ea): return [f"FPR({i.frD}).fpr = FPR({i.frA}).fpr * FPR({i.frC}).fpr + FPR({i.frB}).fpr;"]
    def op_fmsub(self, i, ea): return [f"FPR({i.frD}).fpr = FPR({i.frA}).fpr * FPR({i.frC}).fpr - FPR({i.frB}).fpr;"]
    def op_fnmadd(self, i, ea): return [f"FPR({i.frD}).fpr = -(FPR({i.frA}).fpr * FPR({i.frC}).fpr + FPR({i.frB}).fpr);"]
    def op_fnmsub(self, i, ea): return [f"FPR({i.frD}).fpr = -(FPR({i.frA}).fpr * FPR({i.frC}).fpr - FPR({i.frB}).fpr);"]

    def op_fadds(self, i, ea): return self._single(i, f"FPR({i.frA}).fpr + FPR({i.frB}).fpr")
    def op_fsubs(self, i, ea): return self._single(i, f"FPR({i.frA}).fpr - FPR({i.frB}).fpr")
    def op_fdivs(self, i, ea): return self._single(i, f"FPR({i.frA}).fpr / FPR({i.frB}).fpr")
    def op_fmuls(self, i, ea): return self._single(i, f"FPR({i.frA}).fpr * roundTo25BitAccuracy(FPR({i.frC}).fpr)")
    def op_fmadds(self, i, ea): return self._single(i, f"FPR({i.frA}).fpr * roundTo25BitAccuracy(FPR({i.frC}).fpr) + FPR({i.frB}).fpr")
    def op_fnmadds(self, i, ea): return self._single(i, f"-(FPR({i.frA}).fpr * roundTo25BitAccuracy(FPR({i.frC}).fpr) + FPR({i.frB}).fpr)")
    def op_fmsubs(self, i, ea): return self._single(i, f"FPR({i.frA}).fp0 * roundTo25BitAccuracy(FPR({i.frC}).fp0) - FPR({i.frB}).fp0")
    def op_fnmsubs(self, i, ea): return self._single(i, f"-(FPR({i.frA}).fp0 * roundTo25BitAccuracy(FPR({i.frC}).fp0) - FPR({i.frB}).fp0)")

    def op_frsp(self, i, ea): return self._single(i, f"FPR({i.frB}).fpr")

    def op_fres(self, i, ea):
        return [f"FPR({i.frD}).fpr = fres_espresso(FPR({i.frB}).fpr); FPR({i.frD}).fp1 = FPR({i.frD}).fp0;"]

    def op_frsqrte(self, i, ea):
        return [f"FPR({i.frD}).fpr = frsqrte_espresso(FPR({i.frB}).fpr);"]

    def op_fsel(self, i, ea):
        return [f"FPR({i.frD}) = FPR({i.frA}).fp0 >= -0.0f ? FPR({i.frC}) : FPR({i.frB});"]

    def op_fctiwz(self, i, ea): return [f"FPR({i.frD}).guint = fctiw_result(FPR({i.frB}).fpr, true);"]
    def op_fctiw(self, i, ea): return [f"FPR({i.frD}).guint = fctiw_result(FPR({i.frB}).fpr, false);"]

    def op_fcmpu(self, i, ea):
        return [f"rt_fcmpu(ctx, {i.crfD * 4}, FPR({i.frA}).fp0, FPR({i.frB}).fp0);"]

    def op_mffs(self, i, ea): return [f"FPR({i.frD}).guint = (uint64)ctx->fpscr;"]

    # paired singles (PPCInterpreterPS.cpp) -------------------------------------------------------
    def _ps2(self, i, e0, e1, ftz=False):
        w = (lambda e: f"flushDenormalToZero((float)({e}))") if ftz else (lambda e: f"(float)({e})")
        return [f"{{ double s0 = {w(e0)}; double s1 = {w(e1)};",
                f"\tFPR({i.frD}).fp0 = s0; FPR({i.frD}).fp1 = s1; }}"]

    def op_ps_add(self, i, ea):
        return self._ps2(i, f"FPR({i.frA}).fp0 + FPR({i.frB}).fp0", f"FPR({i.frA}).fp1 + FPR({i.frB}).fp1")

    def op_ps_sub(self, i, ea):
        return self._ps2(i, f"FPR({i.frA}).fp0 - FPR({i.frB}).fp0", f"FPR({i.frA}).fp1 - FPR({i.frB}).fp1")

    def op_ps_div(self, i, ea):
        return self._ps2(i, f"FPR({i.frA}).fp0 / FPR({i.frB}).fp0", f"FPR({i.frA}).fp1 / FPR({i.frB}).fp1")

    def op_ps_mul(self, i, ea):
        return self._ps2(i, f"FPR({i.frA}).fp0 * roundTo25BitAccuracy(FPR({i.frC}).fp0)",
                         f"FPR({i.frA}).fp1 * roundTo25BitAccuracy(FPR({i.frC}).fp1)", ftz=True)

    def op_ps_madd(self, i, ea):
        return self._ps2(i, f"(float)(FPR({i.frA}).fp0 * roundTo25BitAccuracy(FPR({i.frC}).fp0)) + FPR({i.frB}).fp0",
                         f"(float)(FPR({i.frA}).fp1 * roundTo25BitAccuracy(FPR({i.frC}).fp1)) + FPR({i.frB}).fp1", ftz=True)

    def op_ps_nmadd(self, i, ea):
        return self._ps2(i, f"-(FPR({i.frA}).fp0 * roundTo25BitAccuracy(FPR({i.frC}).fp0) + FPR({i.frB}).fp0)",
                         f"-(FPR({i.frA}).fp1 * roundTo25BitAccuracy(FPR({i.frC}).fp1) + FPR({i.frB}).fp1)")

    def op_ps_msub(self, i, ea):
        return self._ps2(i, f"FPR({i.frA}).fp0 * roundTo25BitAccuracy(FPR({i.frC}).fp0) - FPR({i.frB}).fp0",
                         f"FPR({i.frA}).fp1 * roundTo25BitAccuracy(FPR({i.frC}).fp1) - FPR({i.frB}).fp1")

    def op_ps_nmsub(self, i, ea):
        return self._ps2(i, f"-(FPR({i.frA}).fp0 * roundTo25BitAccuracy(FPR({i.frC}).fp0) - FPR({i.frB}).fp0)",
                         f"-(FPR({i.frA}).fp1 * roundTo25BitAccuracy(FPR({i.frC}).fp1) - FPR({i.frB}).fp1)")

    def _ps_s(self, i, half, madd):
        c = f"roundTo25BitAccuracy(FPR({i.frC}).fp{half})"
        add0 = f" + FPR({i.frB}).fp0" if madd else ""
        add1 = f" + FPR({i.frB}).fp1" if madd else ""
        return [f"{{ double c = {c};"] + ["\t" + l for l in self._ps2(i, f"FPR({i.frA}).fp0 * c{add0}", f"FPR({i.frA}).fp1 * c{add1}")] + ["}"]

    def op_ps_muls0(self, i, ea): return self._ps_s(i, 0, False)
    def op_ps_muls1(self, i, ea): return self._ps_s(i, 1, False)
    def op_ps_madds0(self, i, ea): return self._ps_s(i, 0, True)
    def op_ps_madds1(self, i, ea): return self._ps_s(i, 1, True)

    def op_ps_sum0(self, i, ea):
        return self._ps2(i, f"FPR({i.frA}).fp0 + FPR({i.frB}).fp1", f"FPR({i.frC}).fp1")

    def op_ps_sum1(self, i, ea):
        return self._ps2(i, f"FPR({i.frC}).fp0", f"FPR({i.frA}).fp0 + FPR({i.frB}).fp1")

    def op_ps_sel(self, i, ea):
        return [f"{{ double s0 = FPR({i.frA}).fp0 >= -0.0f ? FPR({i.frC}).fp0 : FPR({i.frB}).fp0;",
                f"\tdouble s1 = FPR({i.frA}).fp1 >= -0.0f ? FPR({i.frC}).fp1 : FPR({i.frB}).fp1;",
                f"\tFPR({i.frD}).fp0 = s0; FPR({i.frD}).fp1 = s1; }}"]

    def op_ps_mr(self, i, ea):
        return [f"{{ FPR_t t = FPR({i.frB}); FPR({i.frD}).fp0 = t.fp0; FPR({i.frD}).fp1 = t.fp1; }}"]

    def op_ps_neg(self, i, ea):
        return [f"{{ double s0 = -FPR({i.frB}).fp0; double s1 = -FPR({i.frB}).fp1; FPR({i.frD}).fp0 = s0; FPR({i.frD}).fp1 = s1; }}"]

    def op_ps_abs(self, i, ea):
        return [f"FPR({i.frD}).fp0int = FPR({i.frB}).fp0int & ~(1ULL << 63); FPR({i.frD}).fp1int = FPR({i.frB}).fp1int & ~(1ULL << 63);"]

    def op_ps_nabs(self, i, ea):
        return [f"FPR({i.frD}).fp0int = FPR({i.frB}).fp0int | (1ULL << 63); FPR({i.frD}).fp1int = FPR({i.frB}).fp1int | (1ULL << 63);"]

    def op_ps_res(self, i, ea):
        return self._ps2(i, f"fres_espresso(FPR({i.frB}).fp0)", f"fres_espresso(FPR({i.frB}).fp1)")

    def op_ps_rsqrte(self, i, ea):
        return self._ps2(i, f"frsqrte_espresso(FPR({i.frB}).fp0)", f"frsqrte_espresso(FPR({i.frB}).fp1)")

    def _merge(self, i, h0, h1):
        return [f"{{ double s0 = FPR({i.frA}).fp{h0}; double s1 = FPR({i.frB}).fp{h1}; FPR({i.frD}).fp0 = s0; FPR({i.frD}).fp1 = s1; }}"]

    def op_ps_merge00(self, i, ea): return self._merge(i, 0, 0)
    def op_ps_merge01(self, i, ea): return self._merge(i, 0, 1)
    def op_ps_merge10(self, i, ea): return self._merge(i, 1, 0)
    def op_ps_merge11(self, i, ea): return self._merge(i, 1, 1)

    def op_ps_cmpu0(self, i, ea):
        return [f"rt_fcmpu(ctx, {i.crfD * 4}, FPR({i.frA}).fp0, FPR({i.frB}).fp0);"]

    def op_ps_cmpu1(self, i, ea):
        return [f"rt_fcmpu(ctx, {i.crfD * 4}, FPR({i.frA}).fp1, FPR({i.frB}).fp1);"]

    def op_ps_cmpo0(self, i, ea):
        # Cemu's PS_CMPO0 is fcmpu without the VXSNAN update
        return [f"{{ uint32 fpscr = ctx->fpscr; rt_fcmpu(ctx, {i.crfD * 4}, FPR({i.frA}).fp0, FPR({i.frB}).fp0);",
                "\tctx->fpscr = (ctx->fpscr & ~FPSCR_VXSNAN) | (fpscr & FPSCR_VXSNAN); }"]

    # condition register -------------------------------------------------------------------------
    def _crop(self, i, expr):
        return [f"CRB({i.crbD}) = ({expr}) & 1;"]

    def op_crand(self, i, ea): return self._crop(i, f"CRB({i.crbA}) & CRB({i.crbB})")
    def op_cror(self, i, ea): return self._crop(i, f"CRB({i.crbA}) | CRB({i.crbB})")
    def op_crxor(self, i, ea): return self._crop(i, f"CRB({i.crbA}) ^ CRB({i.crbB})")
    def op_crnand(self, i, ea): return self._crop(i, f"~(CRB({i.crbA}) & CRB({i.crbB}))")
    def op_crnor(self, i, ea): return self._crop(i, f"~(CRB({i.crbA}) | CRB({i.crbB}))")
    def op_creqv(self, i, ea): return self._crop(i, f"~(CRB({i.crbA}) ^ CRB({i.crbB}))")
    def op_crandc(self, i, ea): return self._crop(i, f"CRB({i.crbA}) & ~CRB({i.crbB})")
    def op_crorc(self, i, ea): return self._crop(i, f"CRB({i.crbA}) | ~CRB({i.crbB})")

    def op_mcrf(self, i, ea):
        return [f"memmove(ctx->cr + {i.crfD * 4}, ctx->cr + {i.crfS * 4}, 4);"]

    def op_mfcr(self, i, ea):
        return [f"GPR({i.rT}) = cr_pack(ctx);"]

    def op_mtcrf(self, i, ea):
        out = [f"{{ uint32 s = GPR({i.rS});"]
        for f in range(8):
            if i.crm & (0x80 >> f):
                out += [f"\tCRB({f * 4 + b}) = (s >> {31 - (f * 4 + b)}) & 1;" for b in range(4)]
        return out + ["}"]

    # special registers ----------------------------------------------------------------------------
    SPR = {8: "ctx->spr.LR", 9: "ctx->spr.CTR", **{896 + n: f"ctx->spr.UGQR[{n}]" for n in range(8)}}

    def op_mfspr(self, i, ea):
        if i.spr == 1:
            return [f"GPR({i.rT}) = PPCInterpreter_getXER(ctx);"]
        return [f"GPR({i.rT}) = {self.SPR[i.spr]};"]

    def op_mtspr(self, i, ea):
        if i.spr == 1:
            return [f"PPCInterpreter_setXER(ctx, GPR({i.rT}));"]
        return [f"{self.SPR[i.spr]} = GPR({i.rT});"]

    # traps ----------------------------------------------------------------------------------
    def op_tw(self, i, ea):
        return [f"rt_trap(ctx, {hx(ea)});"]

    op_twi = op_tw

    # branches ---------------------------------------------------------------------------------
    @staticmethod
    def _cond(bo, bi, ctr):
        """C condition for BO/BI (the CTR decrement, if any, is emitted before it)."""
        parts = []
        if ctr and not bo & 4:
            parts.append("ctx->spr.CTR == 0" if bo & 2 else "ctx->spr.CTR != 0")
        if not bo & 16:
            parts.append(f"CRB({bi})" if bo & 8 else f"!CRB({bi})")
        return " && ".join(parts) if parts else None

    def op_b(self, i, ea):
        target = (i.li if i.aa else ea + i.li) & 0xFFFFFFFF
        return self.flow.branch_to(ea, target, None, bool(i.lk))

    def op_bc(self, i, ea):
        pre = [] if i.bo & 4 else ["ctx->spr.CTR--;"]
        target = (i.bd if i.aa else ea + i.bd) & 0xFFFFFFFF
        return pre + self.flow.branch_to(ea, target, self._cond(i.bo, i.bi, True), bool(i.lk))

    def op_bclr(self, i, ea):
        pre = [] if i.bo & 4 else ["ctx->spr.CTR--;"]
        return pre + self.flow.branch_lr(ea, self._cond(i.bo, i.bi, True), bool(i.lk))

    def op_bcctr(self, i, ea):
        return self.flow.branch_ctr(ea, self._cond(i.bo, i.bi, False), bool(i.lk))
