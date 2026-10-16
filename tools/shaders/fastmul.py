"""Find the multiplies of a translated shader that may be written plainly, with no change to any result.

Usage (on the worker, where the shaders' GLSL is: WWHD_RENDER_SHADERS=DIR writes it):
    python3 tools/shaders/fastmul.py GLSL...            # a line per shader: the plain multiplies and what they need
    python3 tools/shaders/fastmul.py -v GLSL            # and why each multiply stays exact
    python3 tools/shaders/fastmul.py --cpp [--float=KEY:PS0,PS1] GLSL...
                                                        # src/gpu/vk/fastmul_table.inc's entries (the renderer's)

Latte's multiply gives 0 when either operand is 0, even against inf or NaN; the translation writes it as
mul_nonIEEE(a, b) (`a == 0 || b == 0 ? 0 : a*b`), and on Adreno those zero tests are ~15% of the GPU's time
(docs/research/graphics-rework.md). A plain a*b differs from it in two cases only: an operand is 0 and the other
inf or NaN (0 against NaN), or an operand is 0 and the product's zero takes a sign (-0 against +0). So a multiply
may be plain when both hold:
  1. its operands can't make the first case: both provably finite, or one provably finite and nonzero;
  2. a zero's sign can't reach anything that tells -0 from +0: every use of the product, followed through the
     shader, meets an absorber first (a nonzero addend, an exact multiply, exp2/log2, a comparison, a texture
     coordinate, an export to a target with no sign bit) and never a divisor, a bit cast or a function this
     doesn't model.
Values are proved with intervals: literals; the shader's uniforms within +-2^32 and finite (a check the renderer
makes on each draw's values before it binds the plain version), nonzero where a proof needs it; samples of units
whose format the renderer checks is normalized ([0,1] or [-1,1]); clamps to literal bounds of values that are never
a NaN (FClamp of a NaN is undefined, and may be the NaN); and what arithmetic makes of those without overflowing.
Anything else (interpolants, float textures: --float=KEY:PS0,... marks a shader's, integer registers) is unknown.
The renderer writes a plain multiply precise (no fma: see draw.cpp FastMulVariant).
Anything this can't parse leaves the whole shader exact.

Output, a line per shader with plain multiplies (no game content: indices and register names):
    KEY ps|vs plain I,J,... finite U.c,... nonzero U.c,... tex N,... exports unsigned
I counts the shader's mul_nonIEEE( calls in its main(), from 0, in textual order (a call inside another's
arguments after it); a call is plain only when every component it is read for is.
"""
import math
import re
import sys

FLT_MAX = 3.4028234663852886e38
FLT_MIN = 1.1754943508222875e-38                       # the smallest normal: a denormal may be flushed to 0
UNI_BOUND = 2.0 ** 32


class ParseError(Exception):
    pass


# ---- expressions -------------------------------------------------------------------------------------------
TOKEN = re.compile(r"\s*(?:(0x[0-9a-fA-F]+)|(\d+\.\d*(?:[eE][+-]?\d+)?F?|\d+(?:[eE][+-]?\d+)F?|\d+)|([A-Za-z_][A-Za-z0-9_]*)|"
                   r"(==|!=|>=|<=|&&|\|\||<<|>>|[-+*/%().,\[\]<>!?:&|^~]))")


def tokens(text):
    out, pos = [], 0
    text = text.strip()
    while pos < len(text):
        m = TOKEN.match(text, pos)
        if not m or m.end() == pos:
            raise ParseError("token at: " + text[pos:pos + 20])
        hexv, num, ident, op = m.groups()
        out.append(("num", float(num.rstrip("F"))) if num else ("hex", int(hexv, 16)) if hexv else ("id", ident) if ident else ("op", op))
        pos = m.end()
    return out


MUL_COUNTER = [0]                                       # mul_nonIEEE( calls so far in this shader's main(), in textual order


class Parser:
    def __init__(self, toks):
        self.t, self.i = toks, 0

    def peek(self):
        return self.t[self.i] if self.i < len(self.t) else ("end", None)

    def take(self, kind=None, val=None):
        tok = self.peek()
        if (kind and tok[0] != kind) or (val is not None and tok[1] != val):
            raise ParseError(f"expected {kind} {val}, got {tok}")
        self.i += 1
        return tok

    def done(self):
        if self.i != len(self.t):
            raise ParseError(f"left over: {self.t[self.i:]}")

    # C's precedence, lowest first: ?:, ||, &&, |, ^, &, == !=, < <= > >=, << >>, + -, * / %, unary. The comparisons
    # and logic are "cmp" (a zero's sign can't reach past them); the bitwise ones "bin" with ops no proof accepts
    def expr(self):
        cond = self.level(0)
        if self.peek() == ("op", "?"):
            self.take()
            a = self.expr()
            self.take("op", ":")
            b = self.expr()
            return ("call", "?:", [cond, a, b])
        return cond

    LEVELS = [("cmp", ("||",)), ("cmp", ("&&",)), ("bin", ("|",)), ("bin", ("^",)), ("bin", ("&",)), ("cmp", ("==", "!=")),
              ("cmp", ("<", "<=", ">", ">=")), ("bin", ("<<", ">>"))]

    def level(self, i):
        if i == len(self.LEVELS):
            return self.sum()
        kind, ops = self.LEVELS[i]
        left = self.level(i + 1)
        while self.peek()[0] == "op" and self.peek()[1] in ops:
            op = self.take()[1]
            left = (kind, op, left, self.level(i + 1))
        return left

    def sum(self):
        left = self.product()
        while self.peek() in (("op", "+"), ("op", "-")):
            op = self.take()[1]
            left = ("bin", op, left, self.product())
        return left

    def product(self):
        left = self.unary()
        while self.peek() in (("op", "*"), ("op", "/"), ("op", "%")):
            op = self.take()[1]
            left = ("bin", op, left, self.unary())
        return left

    def unary(self):
        if self.peek() == ("op", "-"):
            self.take()
            return ("neg", self.unary())
        if self.peek() in (("op", "!"), ("op", "~")):
            self.take()
            return ("not", self.unary())
        return self.postfix()

    def postfix(self):
        e = self.primary()
        while self.peek() == ("op", "."):
            self.take()
            e = ("swz", e, self.take("id")[1])
        return e

    def primary(self):
        tok = self.take()
        if tok[0] == "num":
            return ("num", tok[1])
        if tok == ("op", "("):
            e = self.expr()
            self.take("op", ")")
            return e
        if tok[0] != "id":
            raise ParseError(f"unexpected {tok}")
        name = tok[1]
        if name in ("true", "false"):
            return ("bool", name)
        if self.peek() == ("op", "("):
            self.take()
            textual = None
            if name == "mul_nonIEEE":
                textual = MUL_COUNTER[0]
                MUL_COUNTER[0] += 1
            args = []
            if self.peek() != ("op", ")"):
                args.append(self.expr())
                while self.peek() == ("op", ","):
                    self.take()
                    args.append(self.expr())
            self.take("op", ")")
            if name == "intBitsToFloat" and len(args) == 1 and args[0][0] == "hexlit":
                return ("num", struct_float(args[0][1]))
            if name == "intBitsToFloat" and len(args) == 1 and args[0][0] == "index" and args[0][1].startswith("uf_remapped"):
                return ("uni", args[0][1], args[0][2], args[0][3])  # the renderer checks uf_remapped's components alone
            if textual is not None:
                return ("tmul", textual, args)
            return ("call", name, args)
        if self.peek() == ("op", "["):
            self.take()
            idx_e = self.expr()
            self.take("op", "]")
            if idx_e[0] != "num":                       # an index computed at run time: its element unknown
                if self.peek() == ("op", "."):
                    self.take()
                    self.take("id")
                return ("unknown", name)
            idx = ("num", idx_e[1])
            comp = None
            if self.peek() == ("op", "."):
                self.take()
                comp = self.take("id")[1]
            return ("index", name, int(idx[1]), comp)
        return ("name", name)


def struct_float(bits):
    import struct
    return struct.unpack("<f", struct.pack("<I", bits & 0xFFFFFFFF))[0]


def parse_expr(text):
    toks = tokens(text)
    toks = [("hexlit", t[1]) if t[0] == "hex" else t for t in toks]
    p = Parser([(("num", None) if False else t) for t in toks])
    # hex literals only appear as intBitsToFloat(0x...) or integer operands
    p.t = [t if t[0] != "hexlit" else ("id", "__hex%x" % t[1]) for t in p.t]
    e = p.expr()
    p.done()
    return fix_hex(e)


def fix_hex(e):
    if not isinstance(e, tuple):
        return e
    if e[0] == "name" and e[1].startswith("__hex"):
        return ("hexlit", int(e[1][5:], 16))
    if e[0] == "call" and e[1] == "intBitsToFloat" and len(e[2]) == 1:
        a = fix_hex(e[2][0])
        if a[0] == "hexlit":
            return ("num", struct_float(a[1]))
        return ("call", e[1], [a])
    return tuple(fix_hex(x) if isinstance(x, tuple) else [fix_hex(y) for y in x] if isinstance(x, list) else x for x in e)


# ---- the shader's main() as a list of statements -------------------------------------------------------------
ASSIGN = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*(?:\[\d+\])?)(?:\.([xyzw]+))?\s*(=|\+=|-=|\*=|/=|&=|\|=)\s*(.*);$")
DECL = re.compile(r"^(?:vec4|ivec4|float|int|bool|uvec4|uint)\b")


def statements(lines):
    """main()'s body (between `void main()\\n{` and its `}`) as nested statements"""
    MUL_COUNTER[0] = 0
    out, stack = [], []
    cur = out
    closed = None                                       # the if whose block just closed (an else may follow)
    for raw in lines:
        line = raw.strip()
        if not line or line.startswith("//"):
            continue
        if DECL.match(line):
            continue
        if line.startswith("if(") and line.endswith("{"):
            node = ["if", parse_expr(line[2:-1].strip()), [], None]
            cur.append(node)
            stack.append((cur, node))
            cur = node[2]
            continue
        if line == "}":
            if not stack:
                return out
            cur, closed = stack.pop()
            continue
        if line == "else {":
            if closed is None or closed[0] != "if" or closed[3] is not None or not cur or cur[-1] is not closed:
                raise ParseError("else after something not an if")
            closed[3] = []
            stack.append((cur, closed))
            cur = closed[3]
            closed = None
            continue
        closed = None
        if line.startswith("}") and line.endswith("else {"):
            cur, node = stack.pop()
            node[3] = []
            stack.append((cur, node))
            cur = node[3]
            continue
        if line.startswith("if(") and line.endswith("discard;"):
            cond = line[line.index("(") + 1:line.rindex(")")].strip()
            cur.append(["use", parse_expr(cond)])
            continue
        m = re.match(r"^if\((.*)\)\s*(tempResultf)\s*=\s*(.*);$", line)
        if m and "isinf" in m.group(1):
            cur.append(["guard", m.group(2), parse_expr(m.group(3))])
            continue
        m = re.match(r"^SET_POSITION\((.*)\);$", line)
        if m:                                           # the position: its w divides, so no zero's sign may reach it
            cur.append(["set", "gl_Position", None, "=", parse_expr(m.group(1))])
            continue
        m = re.match(r"^if\((.*?)\)\s+([A-Za-z_][A-Za-z0-9_]*(?:\.[xyzw]+)?\s*(?:=|\+=|-=|\*=|/=|&=|\|=)\s*.*;)$", line)
        if m:                                           # a one-statement if: a block of its own
            inner = ASSIGN.match(m.group(2))
            if not inner:
                raise ParseError("statement: " + line)
            cur.append(["if", parse_expr(m.group(1)), [["set", inner.group(1), inner.group(2), inner.group(3),
                                                       parse_expr(inner.group(4))]], None])
            continue
        m = ASSIGN.match(line)
        if not m:
            raise ParseError("statement: " + line)
        cur.append(["set", m.group(1), m.group(2), m.group(3), parse_expr(m.group(4))])
    raise ParseError("no end of main")


# ---- values ------------------------------------------------------------------------------------------------
class Def:
    __slots__ = ("id", "expr", "uses", "kind")

    def __init__(self, i, expr, kind="value"):
        self.id, self.expr, self.uses, self.kind = i, expr, [], kind


class Shader:
    def __init__(self, text):
        main = text[text.index("void main()"):]
        body = main.split("\n")[2:]
        self.stmts = statements(body)
        if MUL_COUNTER[0] != main.count("mul_nonIEEE("):       # every call numbered, as draw.cpp numbers them
            raise ParseError(f"{MUL_COUNTER[0]} calls parsed of {main.count('mul_nonIEEE(')}")
        self.defs, self.env, self.muls, self.sinks = [], {}, [], []
        self.walk(self.stmts)

    def new(self, expr, kind="value"):
        d = Def(len(self.defs), expr, kind)
        self.defs.append(d)
        return d

    def read(self, name, comp):
        key = (name, comp)
        if key not in self.env:
            if name.startswith("passParameterSem") or name.startswith("gl_"):
                self.env[key] = self.new(("unknown", name), "input")
            elif name in ("tempResultf", "backupReg0f", "backupReg1f", "backupReg2f", "backupReg3f", "backupReg4f") or \
                    name.startswith(("R", "PV", "PS", "tempf", "tempi", "ARi")):
                self.env[key] = self.new(("num", 0.0), "init")      # declared = 0.0 / vec4(0.0)
            else:
                self.env[key] = self.new(("unknown", name), "input")
        return ("def", self.env[key].id)

    def scalar(self, e, k):
        """component k (0..3) of expression e, with names read as definitions"""
        t = e[0]
        if t == "num" or t == "uni" or t == "hexlit" or t == "bool" or t == "unknown":
            return e
        if t == "name":
            return self.read(e[1], "xyzw"[k] if not is_scalar_name(e[1]) else "")
        if t == "index":
            return ("unknown", e[1])                    # an integer read (a uniform's bits, an array): no value proved
        if t == "swz":
            base, sw = e[1], e[2]
            if k >= len(sw):
                raise ParseError("swizzle")
            ck = "xyzw".index(sw[k]) if sw[k] in "xyzw" else "rgba".index(sw[k])
            if base[0] == "name":
                return self.read(base[1], "xyzw"[ck])
            return self.scalar(base, ck)
        if t == "neg":
            return ("neg", self.scalar(e[1], k))
        if t == "not":
            return ("unknown", "not")
        if t == "bin":
            return ("bin", e[1], self.scalar(e[2], k), self.scalar(e[3], k))
        if t == "cmp":
            return ("cmp", e[1], self.scalar(e[2], k), self.scalar(e[3], k))
        if t == "call":
            name, args = e[1], e[2]
            if name in ("vec2", "vec3", "vec4", "ivec2", "ivec3", "ivec4", "uvec4"):
                if len(args) == 1 and arity(args[0]) == 1:
                    return self.scalar(args[0], 0)             # a splat
                flat = [(a, j) for a in args for j in range(arity(a))]
                if k >= len(flat):
                    raise ParseError("constructor")
                a, j = flat[k]
                return self.scalar(a, j)
            if name in ("texture", "textureLod", "textureGather", "texelFetch", "textureGrad"):
                unit = args[0][1] if args[0][0] == "name" else "?"
                coords = [self.scalar(a, j) for a in args[1:] for j in range(arity(a))]
                return ("tex", unit, k, coords)
            return ("call", name, [self.scalar(a, k if arity(a) > 1 else 0) for a in args])
        if t == "tmul":
            if len(e[2]) != 2:
                raise ParseError("mul_nonIEEE arity")
            idx = len(self.muls)                        # this instance; the textual call it is: self.muls[idx]
            self.muls.append(e[1])
            return ("mul", idx, self.scalar(e[2][0], 0), self.scalar(e[2][1], 0))
        if t == "hexlit":
            return e
        raise ParseError("expression " + str(t))

    def assign(self, name, comps, op, rhs):
        comps = comps or ("" if is_scalar_name(name) else "xyzw")
        for i, c in enumerate(comps or [""]):
            k = i if comps else 0
            value = self.scalar(rhs, k if arity(rhs) > 1 else 0)
            if op != "=":
                value = ("bin", op[0], self.read(name, c), value)
            if name.startswith("passPixelColor") or name in ("gl_FragDepth", "gl_Position"):
                self.sinks.append((name, value))
                continue
            d = self.new(value)
            self.env[(name, c)] = d

    def walk(self, stmts):
        for s in stmts:
            if s[0] == "set":
                _, name, comps, op, rhs = s
                self.assign(name, comps, op, rhs)
            elif s[0] == "use":
                self.sinks.append(("use", self.scalar(s[1], 0)))
            elif s[0] == "guard":
                prev = self.read(s[1], "")
                d = self.new(("guard", prev, self.scalar(s[2], 0)))
                self.env[(s[1], "")] = d
            elif s[0] == "if":
                _, cond, then, other = s
                self.sinks.append(("use", self.scalar(cond, 0)))
                before = dict(self.env)
                self.walk(then)
                after_then = self.env
                self.env = dict(before)
                if other is not None:
                    self.walk(other)
                after_else = self.env
                merged = dict(before)
                for key in set(after_then) | set(after_else):
                    a, b = after_then.get(key), after_else.get(key)
                    if a is b:
                        merged[key] = a
                    else:
                        pa = a if a is not None else before.get(key)
                        pb = b if b is not None else before.get(key)
                        ids = [x.id for x in (pa, pb) if x is not None]
                        merged[key] = self.new(("phi", [("def", i) for i in ids]))
                self.env = merged


def is_scalar_name(name):
    return name.startswith(("PV", "PS", "backupReg", "tempResult")) and not name.startswith(("PV0f", "PV1f")) or \
        bool(re.match(r"^PV[01]f[xyzw]$", name)) or name in ("PS0f", "PS1f", "tempResultf", "tempResulti")


def arity(e):
    t = e[0]
    if t == "swz":
        return len(e[2])
    if t == "call" and e[1] in ("vec2", "vec3", "vec4", "ivec2", "ivec3", "ivec4", "uvec4"):
        return int(e[1][-1])
    if t == "call" and e[1] in ("texture", "textureLod"):
        return 4
    if t == "name":
        return 1 if is_scalar_name(e[1]) else 4
    if t in ("bin", "cmp"):
        return max(arity(e[2]), arity(e[3]))
    if t == "neg":
        return arity(e[1])
    if t == "tmul":
        return 1
    if t == "call":
        return max([arity(a) for a in e[2]] or [1])
    return 1


# ---- intervals ---------------------------------------------------------------------------------------------
class Prover:
    def __init__(self, sh, float_units=()):
        self.sh, self.memo, self.nn_memo, self.float_units = sh, {}, {}, set(float_units)
        self.req_finite, self.req_nonzero, self.req_tex = set(), set(), set()
        self.relied_finite, self.relied_tex = set(), set()

    def iv(self, e):
        """an interval [lo, hi] the value is always in (finite bounds), or None"""
        t = e[0]
        if t == "num":
            return (e[1], e[1]) if math.isfinite(e[1]) else None
        if t == "uni":
            return (-UNI_BOUND, UNI_BOUND)
        if t == "def":
            i = e[1]
            if i not in self.memo:
                self.memo[i] = None                     # a cycle (phi) stays unknown
                self.memo[i] = self.iv(self.sh.defs[i].expr)
            return self.memo[i]
        if t == "neg":
            a = self.iv(e[1])
            return (-a[1], -a[0]) if a else None
        if t == "tex":
            if e[1] in self.float_units:                # a float format: anything, inf and NaN included
                return None
            return (-1.0, 1.0)                          # with a normalized format, checked on each draw (req_tex)
        if t in ("bin", "mul"):
            op = e[1] if t == "bin" else "*"
            a, b = self.iv(e[2]), self.iv(e[3])
            if a is None or b is None:
                return None
            if op == "+":
                r = (a[0] + b[0], a[1] + b[1])
            elif op == "-":
                r = (a[0] - b[1], a[1] - b[0])
            elif op == "*":
                p = [a[0] * b[0], a[0] * b[1], a[1] * b[0], a[1] * b[1]]
                r = (min(p), max(p))
            elif op == "/":
                if b[0] <= 0.0 <= b[1]:
                    return None
                p = [a[0] / b[0], a[0] / b[1], a[1] / b[0], a[1] / b[1]]
                r = (min(p), max(p))
            else:
                return None
            r = (r[0] - abs(r[0]) * 1e-6, r[1] + abs(r[1]) * 1e-6)     # rounding
            return r if -FLT_MAX < r[0] and r[1] < FLT_MAX else None
        if t == "call":
            name, args = e[1], e[2]
            if name == "clamp" and len(args) == 3 and args[1][0] == "num" and args[2][0] == "num" and self.notnan(args[0]):
                return (args[1][1], args[2][1])         # FClamp of a NaN is undefined (it may be the NaN): not a NaN
            ivs = [self.iv(a) for a in args]
            if any(x is None for x in ivs):
                return None
            if name == "max" and len(ivs) == 2:
                return (max(ivs[0][0], ivs[1][0]), max(ivs[0][1], ivs[1][1]))
            if name == "min" and len(ivs) == 2:
                return (min(ivs[0][0], ivs[1][0]), min(ivs[0][1], ivs[1][1]))
            if name == "exp2" and ivs[0][1] <= 126:
                return (0.0, 2.0 ** ivs[0][1])
            if name == "log2" and ivs[0][0] > 0:
                return (math.log2(ivs[0][0]), math.log2(ivs[0][1]))
            if name == "sqrt" and ivs[0][0] >= 0:
                return (math.sqrt(ivs[0][0]), math.sqrt(ivs[0][1]))
            if name == "floor":
                return (math.floor(ivs[0][0]), math.floor(ivs[0][1]))
            if name == "fract":
                return (0.0, 1.0)
            if name in ("sin", "cos"):
                return (-1.0, 1.0)
            return None
        if t == "guard":                                # log2(max(0, x)) with its -inf made -FLT_MAX
            prev = self.sh.defs[e[1][1]].expr if e[1][0] == "def" else None
            if prev and prev[0] == "call" and prev[1] == "log2":
                arg = self.iv(prev[2][0])
                if arg is not None and arg[0] >= 0:
                    return (-FLT_MAX, math.log2(max(arg[1], 1e-38)))
            return None
        if t == "phi":
            ivs = [self.iv(x) for x in e[1]]
            if any(x is None for x in ivs):
                return None
            return (min(x[0] for x in ivs), max(x[1] for x in ivs))
        return None

    def notnan(self, e):
        """e is never a NaN (it may be an inf): a finite interval, or ops that make no NaN of their operands"""
        if self.iv(e) is not None:
            return True
        t = e[0]
        if t == "num":
            return not math.isnan(e[1])
        if t == "def":
            i = e[1]
            if i not in self.nn_memo:
                self.nn_memo[i] = False                 # a cycle (phi) stays unknown
                self.nn_memo[i] = self.notnan(self.sh.defs[i].expr)
            return self.nn_memo[i]
        if t == "neg":
            return self.notnan(e[1])
        if t == "phi":
            return all(self.notnan(x) for x in e[1])
        if t == "mul":                                  # the exact multiply: 0 if either is 0, else no 0 * inf
            return self.notnan(e[2]) and self.notnan(e[3])
        if t == "bin" and e[1] in ("+", "-"):           # finite + inf is inf; inf - inf is the NaN
            return (self.iv(e[2]) is not None and self.notnan(e[3])) or (self.iv(e[3]) is not None and self.notnan(e[2]))
        if t == "call" and e[1] in ("clamp", "min", "max"):
            return all(self.notnan(a) for a in e[2])
        return False

    def rely(self, e):
        """a proof used e's interval: what that interval rests on is checked on each draw too"""
        u, t = self.needs(e)
        self.relied_finite |= u
        self.relied_tex |= t

    def leaves(self, e, kind, out):
        if not isinstance(e, tuple):
            return
        if e[0] == kind:
            out.append(e)
        if e[0] == "def" and kind != "def":
            return
        for x in e[1:]:
            if isinstance(x, tuple):
                self.leaves(x, kind, out)
            elif isinstance(x, list):
                for y in x:
                    self.leaves(y, kind, out)

    def needs(self, e, seen=None):
        """the uniforms and texture units an interval of e relies on"""
        seen = seen if seen is not None else set()
        unis, texs = set(), set()
        stack = [e]
        while stack:
            x = stack.pop()
            if not isinstance(x, tuple):
                continue
            if x[0] == "uni":
                unis.add(f"{x[2]}.{x[3]}")
            elif x[0] == "tex":
                if x[1] not in self.float_units:         # a float unit's samples were never given a range
                    texs.add(x[1])
            elif x[0] == "def":
                if x[1] not in seen:
                    seen.add(x[1])
                    stack.append(self.sh.defs[x[1]].expr)
                continue
            for y in x[1:]:
                if isinstance(y, tuple):
                    stack.append(y)
                elif isinstance(y, list):
                    stack.extend(y)
        return unis, texs


# ---- a zero's sign -----------------------------------------------------------------------------------------
ABSORB_CALLS = {"exp2", "log2", "isinf", "isnan", "texture", "textureLod", "textureGrad", "textureGather", "texelFetch"}
PASS_CALLS = {"clamp", "max", "min", "floor", "fract", "sqrt", "sin", "vec2", "vec3", "vec4"}


class Uses:
    """for each definition, every place its value is read: the chain of nodes from it up to the root"""

    def __init__(self, sh):
        self.sh = sh
        self.by_def = {i: [] for i in range(len(sh.defs))}
        for d in sh.defs:
            self.collect(d.expr, [], ("def", d.id))
        for name, value in sh.sinks:
            self.collect(value, [], ("sink", name))

    def collect(self, e, chain, root):
        if not isinstance(e, tuple):
            return
        if e[0] == "def":
            self.by_def[e[1]].append((chain, root))
            return
        children = []
        for pos, x in enumerate(e[1:], 1):
            if isinstance(x, tuple):
                children.append((pos, None, x))
            elif isinstance(x, list):
                for j, y in enumerate(x):
                    children.append((pos, j, y))
        for pos, j, x in children:
            self.collect(x, [(e, pos, j)] + chain, root)


def absorbed(sh, uses, plain, prover, value_def, seen, why):
    """a -0 where the exact multiply gives +0 can't change anything once it's read through every use of def"""
    if value_def in seen:
        return True
    seen.add(value_def)
    for chain, root in uses.by_def[value_def]:
        ok = None
        for node, pos, j in chain:
            t = node[0]
            if t == "neg" or t == "phi":
                continue
            if t == "bin":
                other = node[3] if pos == 2 else node[2]
                if node[1] in "+-":
                    iv = prover.iv(other)
                    if iv and (iv[0] >= FLT_MIN or iv[1] <= -FLT_MIN):
                        prover.rely(other)
                        ok = True
                        break
                    continue
                if node[1] == "*":
                    continue
                if node[1] == "/":
                    if pos == 3:
                        ok = False
                        why.append("a divisor")
                        break
                    continue
            if t == "mul":
                if node[1] in plain:
                    continue
                ok = True                               # the exact multiply gives +0 for either zero
                break
            if t == "cmp":
                ok = True
                break
            if t == "tex":
                ok = True
                break
            if t == "call":
                if node[1] in ABSORB_CALLS:
                    ok = True
                    break
                if node[1] in PASS_CALLS:
                    continue
                ok = False
                why.append("call " + node[1])
                break
            if t == "guard":
                continue
            ok = False
            why.append("node " + t)
            break
        if ok is True:
            continue
        if ok is False:
            return False
        kind, name = root                               # it reached the root unabsorbed
        if kind == "def":
            if not absorbed(sh, uses, plain, prover, name, seen, why):
                return False
        elif name.startswith("passPixelColor"):
            prover.exports = True                        # targets with no sign bit, checked on each draw
        elif name == "use":
            continue
        else:
            why.append("export " + name)
            return False
    return True


def analyse(text, verbose=False, float_units=()):
    sh = Shader(text)
    prover = Prover(sh, float_units)
    prover.exports = False
    muls = {}
    for d in sh.defs:
        found = []
        prover.leaves(d.expr, "mul", found)
        for m in found:
            muls[m[1]] = (m, d.id)
    for _, value in sh.sinks:
        found = []
        prover.leaves(value, "mul", found)
        for m in found:
            muls.setdefault(m[1], (m, None))
    # rule 1: the operands
    cand, reason = {}, {}
    for idx, (m, d) in sorted(muls.items()):
        a, b = prover.iv(m[2]), prover.iv(m[3])
        nz = lambda iv: iv is not None and (iv[0] >= FLT_MIN or iv[1] <= -FLT_MIN)
        if a is not None and b is not None:
            cand[idx] = (m, d, [m[2], m[3]])
        elif a is not None and nz(a):
            cand[idx] = (m, d, [m[2]])
        elif b is not None and nz(b):
            cand[idx] = (m, d, [m[3]])
        elif m[2][0] == "uni" or m[3][0] == "uni":
            u = m[2] if m[2][0] == "uni" else m[3]
            cand[idx] = (m, d, [u])                    # a uniform the draw checks is finite and nonzero
            prover.req_nonzero.add(f"{u[2]}.{u[3]}")
        else:
            reason[idx] = "an operand may be inf or NaN"
    # rule 2: the product's zero sign, to a fixpoint (a multiply made exact again absorbs for the others)
    plain = set(cand)
    uses = Uses(sh)
    changed = True
    while changed:
        changed = False
        for idx in sorted(plain):
            m, d, _ = cand[idx]
            why = []
            if d is None or not absorbed_from_mul(sh, uses, plain, prover, idx, d, why):
                plain.discard(idx)
                reason[idx] = "a zero's sign reaches " + (why[0] if why else "an export")
                changed = True
    for idx in plain:
        for e in cand[idx][2]:
            u, t = prover.needs(e)
            prover.req_finite |= u
            prover.req_tex |= t
    # the absorbers' intervals (recorded during the last, final pass of the fixpoint: a superset is harmless)
    prover.req_finite |= prover.relied_finite
    prover.req_tex |= prover.relied_tex
    # a uniform the nonzero rule leans on, for the multiplies kept plain only
    keep_nz = set()
    for idx in plain:
        for e in cand[idx][2]:
            if e[0] == "uni":
                keep_nz.add(f"{e[2]}.{e[3]}")
    prover.req_nonzero &= keep_nz
    # a textual call is plain only when every instance of it (one per component it was read for) is
    textual = {}
    for i, t in enumerate(sh.muls):
        textual.setdefault(t, []).append(i)
    plain_text = sorted(t for t, inst in textual.items() if all(i in plain for i in inst))
    if verbose:
        for t, inst in sorted(textual.items()):
            why = [reason.get(i, "?") for i in inst if i not in plain]
            print(f"  mul {t}: {'plain' if t in plain_text else 'exact: ' + (why[0] if why else '?')}")
    return plain_text, MUL_COUNTER[0], prover, prover.req_nonzero


def absorbed_from_mul(sh, uses, plain, prover, idx, d, why):
    """the multiply's own place in its definition first, then the definition's uses"""
    for chain, root in [(c, r) for c, r in walk_to(sh.defs[d].expr, idx)]:
        for node, pos, j in chain:
            t = node[0]
            if t in ("neg", "phi"):
                continue
            if t == "bin":
                other = node[3] if pos == 2 else node[2]
                if node[1] in "+-":
                    iv = prover.iv(other)
                    if iv and (iv[0] >= FLT_MIN or iv[1] <= -FLT_MIN):
                        prover.rely(other)
                        return True
                    continue
                if node[1] == "*":
                    continue
                if node[1] == "/":
                    if pos == 3:
                        why.append("a divisor")
                        return False
                    continue
            if t == "mul":
                if node[1] in plain:
                    continue
                return True
            if t in ("cmp", "tex"):
                return True
            if t == "call":
                if node[1] in ABSORB_CALLS:
                    return True
                if node[1] in PASS_CALLS:
                    continue
                why.append("call " + node[1])
                return False
            why.append("node " + t)
            return False
    return absorbed(sh, uses, plain, prover, d, set(), why)


def walk_to(e, idx, chain=None):
    """the chain of nodes above mul idx in e (innermost first)"""
    chain = chain or []
    if not isinstance(e, tuple):
        return
    if e[0] == "mul" and e[1] == idx:
        yield chain, None
        return
    for pos, x in enumerate(e[1:], 1):
        if isinstance(x, tuple):
            yield from walk_to(x, idx, [(e, pos, None)] + chain)
        elif isinstance(x, list):
            for j, y in enumerate(x):
                yield from walk_to(y, idx, [(e, pos, j)] + chain)


def fnv64(data):
    """FNV-1a 64 of the GLSL's bytes as the decompiler wrote them (draw.cpp checks it before rewriting)"""
    h = 0xCBF29CE484222325
    for c in data:
        h = ((h ^ c) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def cpp_line(key, stage, data, plain, prover, nonzero):
    """the renderer's table entry (src/gpu/vk/fastmul_table.inc): key, vertex, the plain calls, the uniform components
    (index * 4 + x/y/z/w) checked finite, those checked nonzero, the texture units checked normalized, the exports"""
    comp = lambda u: int(u.split(".")[0]) * 4 + "xyzw".index(u.split(".")[1])
    units = 0
    for t in prover.req_tex:
        units |= 1 << int(re.sub(r"\D", "", t))
    lst = lambda xs: "{ " + ", ".join(str(x) for x in xs) + " }" if xs else "{}"
    return (f"{{ 0x{key}ull, {'true' if stage == 'vs' else 'false'}, 0x{fnv64(data):016x}ull, {lst(plain)},\n\t{lst(sorted(comp(u) for u in prover.req_finite))}, "
            f"{lst(sorted(comp(u) for u in nonzero))}, 0x{units:x}, {'true' if prover.exports else 'false'} }},")


def main(argv):
    verbose = "-v" in argv
    cpp = "--cpp" in argv
    argv = [a for a in argv if a != "--cpp"]
    floats = {}                                         # --float KEY:UNIT,UNIT (a unit bound to a float format)
    for a in argv:
        if a.startswith("--float="):
            key, units = a[8:].split(":")
            floats[key] = ["textureUnit" + u for u in units.split(",")]
    files = [a for a in argv if a != "-v" and not a.startswith("--float=")]
    for path in files:
        name = path.split("/")[-1]
        key, stage = name.split(".")[0], name.split(".")[1]
        text = open(path).read()
        try:
            plain, total, prover, nonzero = analyse(text, verbose, floats.get(key, ()))
        except ParseError as e:
            print(f"# {key} {stage}: not parsed ({e})")
            continue
        if cpp:
            if plain:
                print(cpp_line(key, stage, open(path, "rb").read(), plain, prover, nonzero))
            continue
        if verbose or plain:
            print(f"{key} {stage} plain {','.join(map(str, plain)) or '-'} of {total}"
                  f" finite {','.join(sorted(prover.req_finite)) or '-'} nonzero {','.join(sorted(nonzero)) or '-'}"
                  f" tex {','.join(sorted(prover.req_tex)) or '-'}{' exports unsigned' if prover.exports else ''}")


if __name__ == "__main__":
    main(sys.argv[1:])
