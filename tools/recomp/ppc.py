"""Espresso (PowerPC 750CL + paired singles) instruction decoder for the recompiler.

decode(word) -> Insn | None. Mnemonics are the architectural base forms: `rc` and `oe` are
fields, not part of the name (so `add.` and `addo` are both "add"), and no simplified mnemonics
(`li`, `mr`, `blr`...) are produced. The generator dispatches on `Insn.op`.

Encodings follow the 750CL user manual (chapter 12 and the paired-single appendix). Where Cemu's
interpreter decodes differently (it folds fcmpo into fcmpu and mcrfs into fcmpu, and lacks
mtfsb0/mtfsfi/ps_cmpo1), that is noted in NOTES and the census flags any use in WWHD.
Standard library only.
"""
from dataclasses import dataclass, field


@dataclass(frozen=True)
class Insn:
    op: str
    word: int
    f: dict = field(default_factory=dict, compare=False)

    def __getattr__(self, k):
        try:
            return self.f[k]
        except KeyError:
            raise AttributeError(k) from None


def bits(w, hi, n):
    """Field of n bits whose most significant bit is IBM bit hi (bit 0 = MSB of the word)."""
    return (w >> (32 - hi - n)) & ((1 << n) - 1)


def sext(v, n):
    return v - (1 << n) if v & (1 << (n - 1)) else v


# ---- field extractors -------------------------------------------------------------------------
def _rD(w): return bits(w, 6, 5)
def _rA(w): return bits(w, 11, 5)
def _rB(w): return bits(w, 16, 5)
def _rC(w): return bits(w, 21, 5)
def _rc(w): return w & 1


def f_D(w):        # rD, rA, SIMM/d
    return dict(rD=_rD(w), rA=_rA(w), simm=sext(w & 0xFFFF, 16))


def f_S_imm(w):    # rS, rA, UIMM (logical immediates)
    return dict(rS=_rD(w), rA=_rA(w), uimm=w & 0xFFFF)


def f_mem(w):      # rD/rS/frD/frS, d(rA)
    return dict(rT=_rD(w), rA=_rA(w), d=sext(w & 0xFFFF, 16))


def f_X(w):        # rT, rA, rB, Rc
    return dict(rT=_rD(w), rA=_rA(w), rB=_rB(w), rc=_rc(w))


def f_XO(w):       # rD, rA, rB, OE, Rc
    return dict(rD=_rD(w), rA=_rA(w), rB=_rB(w), oe=bits(w, 21, 1), rc=_rc(w))


def f_cmp(w):      # crfD, L, rA, rB|SIMM|UIMM
    return dict(crfD=bits(w, 6, 3), L=bits(w, 10, 1), rA=_rA(w), rB=_rB(w),
                simm=sext(w & 0xFFFF, 16), uimm=w & 0xFFFF)


def f_M(w):        # rS, rA, SH|rB, MB, ME, Rc
    return dict(rS=_rD(w), rA=_rA(w), sh=_rB(w), rB=_rB(w), mb=bits(w, 21, 5), me=bits(w, 26, 5), rc=_rc(w))


def f_A(w):        # frD, frA, frB, frC, Rc
    return dict(frD=_rD(w), frA=_rA(w), frB=_rB(w), frC=_rC(w), rc=_rc(w))


def f_fcmp(w):
    return dict(crfD=bits(w, 6, 3), frA=_rA(w), frB=_rB(w))


def f_XL_cr(w):    # crbD, crbA, crbB
    return dict(crbD=_rD(w), crbA=_rA(w), crbB=_rB(w))


def f_bclr(w):     # BO, BI, LK
    return dict(bo=_rD(w), bi=_rA(w), lk=w & 1)


def f_spr(w):      # rT, spr (halves swapped in the encoding)
    return dict(rT=_rD(w), spr=bits(w, 16, 5) << 5 | bits(w, 11, 5))


def f_psq(w):      # psq_l/psq_st: frD, rA, W, I, d (12-bit)
    return dict(frT=_rD(w), rA=_rA(w), W=bits(w, 16, 1), I=bits(w, 17, 3), d=sext(w & 0xFFF, 12))


def f_psqx(w):     # psq_lx/psq_stx: frD, rA, rB, W, I
    return dict(frT=_rD(w), rA=_rA(w), rB=_rB(w), W=bits(w, 21, 1), I=bits(w, 22, 3))


def f_none(w):
    return {}


# ---- opcode tables ----------------------------------------------------------------------------
PRIMARY = {
    3: ("twi", lambda w: dict(to=_rD(w), rA=_rA(w), simm=sext(w & 0xFFFF, 16))),
    7: ("mulli", f_D), 8: ("subfic", f_D),
    10: ("cmpli", f_cmp), 11: ("cmpi", f_cmp),
    12: ("addic", f_D), 13: ("addic.", f_D), 14: ("addi", f_D), 15: ("addis", f_D),
    16: ("bc", lambda w: dict(bo=_rD(w), bi=_rA(w), bd=sext(w & 0xFFFC, 16), aa=bits(w, 30, 1), lk=w & 1)),
    17: ("sc", f_none),
    18: ("b", lambda w: dict(li=sext(w & 0x03FFFFFC, 26), aa=bits(w, 30, 1), lk=w & 1)),
    20: ("rlwimi", f_M), 21: ("rlwinm", f_M), 23: ("rlwnm", f_M),
    24: ("ori", f_S_imm), 25: ("oris", f_S_imm), 26: ("xori", f_S_imm), 27: ("xoris", f_S_imm),
    28: ("andi.", f_S_imm), 29: ("andis.", f_S_imm),
    32: ("lwz", f_mem), 33: ("lwzu", f_mem), 34: ("lbz", f_mem), 35: ("lbzu", f_mem),
    36: ("stw", f_mem), 37: ("stwu", f_mem), 38: ("stb", f_mem), 39: ("stbu", f_mem),
    40: ("lhz", f_mem), 41: ("lhzu", f_mem), 42: ("lha", f_mem), 43: ("lhau", f_mem),
    44: ("sth", f_mem), 45: ("sthu", f_mem), 46: ("lmw", f_mem), 47: ("stmw", f_mem),
    48: ("lfs", f_mem), 49: ("lfsu", f_mem), 50: ("lfd", f_mem), 51: ("lfdu", f_mem),
    52: ("stfs", f_mem), 53: ("stfsu", f_mem), 54: ("stfd", f_mem), 55: ("stfdu", f_mem),
    56: ("psq_l", f_psq), 57: ("psq_lu", f_psq), 60: ("psq_st", f_psq), 61: ("psq_stu", f_psq),
}

OP19 = {  # XL-form, 10-bit XO
    0: ("mcrf", lambda w: dict(crfD=bits(w, 6, 3), crfS=bits(w, 11, 3))),
    16: ("bclr", f_bclr), 528: ("bcctr", f_bclr), 50: ("rfi", f_none), 150: ("isync", f_none),
    33: ("crnor", f_XL_cr), 129: ("crandc", f_XL_cr), 193: ("crxor", f_XL_cr), 225: ("crnand", f_XL_cr),
    257: ("crand", f_XL_cr), 289: ("creqv", f_XL_cr), 417: ("crorc", f_XL_cr), 449: ("cror", f_XL_cr),
}

OP31_XO = {  # XO-form arithmetic, 9-bit XO (bit 21 is OE)
    8: "subfc", 10: "addc", 11: "mulhwu", 40: "subf", 75: "mulhw", 104: "neg", 136: "subfe",
    138: "adde", 200: "subfze", 202: "addze", 232: "subfme", 234: "addme", 235: "mullw", 266: "add",
    459: "divwu", 491: "divw",
}

OP31 = {  # X-form, 10-bit XO
    0: ("cmp", f_cmp), 32: ("cmpl", f_cmp), 4: ("tw", f_X),
    19: ("mfcr", f_X), 144: ("mtcrf", lambda w: dict(rS=_rD(w), crm=bits(w, 12, 8))),
    83: ("mfmsr", f_X), 146: ("mtmsr", f_X), 210: ("mtsr", f_X), 242: ("mtsrin", f_X),
    595: ("mfsr", f_X), 659: ("mfsrin", f_X),
    339: ("mfspr", f_spr), 467: ("mtspr", f_spr), 371: ("mftb", f_spr),
    512: ("mcrxr", lambda w: dict(crfD=bits(w, 6, 3))),
    20: ("lwarx", f_X), 150: ("stwcx.", f_X),
    23: ("lwzx", f_X), 55: ("lwzux", f_X), 87: ("lbzx", f_X), 119: ("lbzux", f_X),
    279: ("lhzx", f_X), 311: ("lhzux", f_X), 343: ("lhax", f_X), 375: ("lhaux", f_X),
    151: ("stwx", f_X), 183: ("stwux", f_X), 215: ("stbx", f_X), 247: ("stbux", f_X),
    407: ("sthx", f_X), 439: ("sthux", f_X),
    534: ("lwbrx", f_X), 662: ("stwbrx", f_X), 790: ("lhbrx", f_X), 918: ("sthbrx", f_X),
    533: ("lswx", f_X), 661: ("stswx", f_X),
    597: ("lswi", lambda w: dict(rT=_rD(w), rA=_rA(w), nb=_rB(w))),
    725: ("stswi", lambda w: dict(rT=_rD(w), rA=_rA(w), nb=_rB(w))),
    535: ("lfsx", f_X), 567: ("lfsux", f_X), 599: ("lfdx", f_X), 631: ("lfdux", f_X),
    663: ("stfsx", f_X), 695: ("stfsux", f_X), 727: ("stfdx", f_X), 759: ("stfdux", f_X),
    983: ("stfiwx", f_X),
    24: ("slw", f_X), 536: ("srw", f_X), 792: ("sraw", f_X), 824: ("srawi", f_X),
    26: ("cntlzw", f_X), 922: ("extsh", f_X), 954: ("extsb", f_X),
    28: ("and", f_X), 60: ("andc", f_X), 124: ("nor", f_X), 284: ("eqv", f_X), 316: ("xor", f_X),
    412: ("orc", f_X), 444: ("or", f_X), 476: ("nand", f_X),
    54: ("dcbst", f_X), 86: ("dcbf", f_X), 246: ("dcbtst", f_X), 278: ("dcbt", f_X), 470: ("dcbi", f_X),
    982: ("icbi", f_X), 1014: ("dcbz", f_X),
    306: ("tlbie", f_X), 566: ("tlbsync", f_none), 598: ("sync", f_none), 854: ("eieio", f_none),
    310: ("eciwx", f_X), 438: ("ecowx", f_X),
}

OP59 = {18: "fdivs", 20: "fsubs", 21: "fadds", 22: "fsqrts", 24: "fres", 25: "fmuls",
        28: "fmsubs", 29: "fmadds", 30: "fnmsubs", 31: "fnmadds"}          # A-form, 5-bit XO

OP63_A = {18: "fdiv", 20: "fsub", 21: "fadd", 22: "fsqrt", 23: "fsel", 25: "fmul", 26: "frsqrte",
          28: "fmsub", 29: "fmadd", 30: "fnmsub", 31: "fnmadd"}            # A-form, 5-bit XO
OP63_X = {  # X-form, 10-bit XO
    0: ("fcmpu", f_fcmp), 32: ("fcmpo", f_fcmp), 12: ("frsp", f_A), 14: ("fctiw", f_A),
    15: ("fctiwz", f_A), 40: ("fneg", f_A), 72: ("fmr", f_A), 136: ("fnabs", f_A), 264: ("fabs", f_A),
    38: ("mtfsb1", lambda w: dict(crbD=_rD(w), rc=_rc(w))), 70: ("mtfsb0", lambda w: dict(crbD=_rD(w), rc=_rc(w))),
    64: ("mcrfs", lambda w: dict(crfD=bits(w, 6, 3), crfS=bits(w, 11, 3))),
    134: ("mtfsfi", lambda w: dict(crfD=bits(w, 6, 3), imm=bits(w, 16, 4), rc=_rc(w))),
    583: ("mffs", f_A), 711: ("mtfsf", lambda w: dict(fm=bits(w, 7, 8), frB=_rB(w), rc=_rc(w))),
}

OP4_A = {10: "ps_sum0", 11: "ps_sum1", 12: "ps_muls0", 13: "ps_muls1", 14: "ps_madds0",
         15: "ps_madds1", 18: "ps_div", 20: "ps_sub", 21: "ps_add", 23: "ps_sel", 24: "ps_res",
         25: "ps_mul", 26: "ps_rsqrte", 28: "ps_msub", 29: "ps_madd", 30: "ps_nmsub", 31: "ps_nmadd"}
OP4_X = {0: ("ps_cmpu0", f_fcmp), 32: ("ps_cmpo0", f_fcmp), 64: ("ps_cmpu1", f_fcmp), 96: ("ps_cmpo1", f_fcmp),
         40: ("ps_neg", f_A), 72: ("ps_mr", f_A), 136: ("ps_nabs", f_A), 264: ("ps_abs", f_A),
         528: ("ps_merge00", f_A), 560: ("ps_merge01", f_A), 592: ("ps_merge10", f_A), 624: ("ps_merge11", f_A),
         1014: ("dcbz_l", f_X)}
OP4_Q = {6: "psq_lx", 7: "psq_stx", 38: "psq_lux", 39: "psq_stux"}      # 6-bit XO

# Where Cemu's interpreter (the M1 oracle) disagrees with the architecture.
NOTES = {
    "fcmpo": "Cemu runs it as fcmpu (same CR result; only FPSCR exception bits differ)",
    "mcrfs": "Cemu decodes it as fcmpu",
    "mtfsb0": "unimplemented in Cemu", "mtfsfi": "unimplemented in Cemu",
    "ps_cmpo1": "unimplemented in Cemu", "fsqrt": "unimplemented in Cemu", "fsqrts": "unimplemented in Cemu",
}


def decode(w):
    """Decode one big-endian instruction word; None if it is not a valid Espresso instruction."""
    p = w >> 26
    if p in PRIMARY:
        name, fx = PRIMARY[p]
        if p == 17 and not (w & 2):
            return None
        return Insn(name, w, fx(w))
    x10 = bits(w, 21, 10)
    x5 = bits(w, 26, 5)
    if p == 19:
        e = OP19.get(x10)
    elif p == 31:
        if bits(w, 22, 9) in OP31_XO:
            return Insn(OP31_XO[bits(w, 22, 9)], w, f_XO(w))
        e = OP31.get(x10)
    elif p == 59:
        return Insn(OP59[x5], w, f_A(w)) if x5 in OP59 else None
    elif p == 63:
        if x5 in OP63_A:
            return Insn(OP63_A[x5], w, f_A(w))
        e = OP63_X.get(x10)
    elif p == 4:
        if bits(w, 25, 6) in OP4_Q:
            return Insn(OP4_Q[bits(w, 25, 6)], w, f_psqx(w))
        if x5 in OP4_A:
            return Insn(OP4_A[x5], w, f_A(w))
        e = OP4_X.get(x10)
    else:
        return None
    return Insn(e[0], w, e[1](w)) if e else None
