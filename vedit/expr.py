"""エフェクトの値に書く式(After Effects のエクスプレッションの小さい版)。eval は使わず、ここで字句・構文を解析して木にする。
使えるもの: 数・変数(t=要素の頭からの秒、dur=要素の長さ、value=元の値、fps、frame、pi)、+ - * / % ^(べき)、( )、比較 < > <= >= == !=、
関数 sin cos tan abs min max clamp floor ceil round sqrt exp log pow lerp step smooth noise wiggle loop rand。
例: "wiggle(3, 20)" / "value + sin(t*6.28)*30" / "loop(0.5) < 0.25" / "clamp(t/0.3, 0, 1)*1.2" """
import math
import random
import re

_TOK = re.compile(r"\s*(?:(\d+\.\d*|\.\d+|\d+)|([A-Za-z_][A-Za-z_0-9]*)|(\*\*|<=|>=|==|!=|[-+*/%^(),<>]))")


class ExprError(ValueError):
    pass


def _lex(s):
    out, i = [], 0
    s = str(s)
    while i < len(s):
        if s[i:].strip() == "":
            break
        m = _TOK.match(s, i)
        if not m:
            raise ExprError(f"式が読めません({i + 1}文字目): {s!r}")
        i = m.end()
        num, name, op = m.groups()
        out.append(("n", float(num)) if num else ("v", name) if name else ("o", "^" if op == "**" else op))
    out.append(("end", None))
    return out


def _noise1(seed, x):
    """なめらかな 1 次元ノイズ(-1〜1)。整数の位置に乱数を置いて smoothstep でつなぐ(seed ごとに固定)"""
    i = math.floor(x)
    f = x - i
    a = random.Random(seed * 1000003 + i).uniform(-1, 1)
    b = random.Random(seed * 1000003 + i + 1).uniform(-1, 1)
    u = f * f * (3 - 2 * f)
    return a + (b - a) * u


def wiggle(seed, t, freq, amp, octaves=1, mult=0.5):
    """AE の wiggle と同じ考え方: 1秒に freq 回くらい揺れ、幅は ±amp。octaves で細かい揺れを重ねる(seed で固定)"""
    v, a, f, tot = 0.0, 1.0, float(freq), 0.0
    for o in range(max(1, int(octaves))):
        v += _noise1(seed + o * 7919, t * f) * a
        tot += a
        a *= float(mult)
        f *= 2
    return v / tot * float(amp)


FUNCS = {
    "sin": (1, 1, math.sin), "cos": (1, 1, math.cos), "tan": (1, 1, math.tan), "abs": (1, 1, abs),
    "floor": (1, 1, math.floor), "ceil": (1, 1, math.ceil), "round": (1, 2, lambda x, n=0: round(x, int(n))),
    "sqrt": (1, 1, lambda x: math.sqrt(max(0.0, x))), "exp": (1, 1, lambda x: math.exp(min(700.0, x))),
    "log": (1, 1, lambda x: math.log(max(1e-300, x))), "pow": (2, 2, lambda a, b: math.pow(a, b)),
    "min": (2, 99, min), "max": (2, 99, max), "clamp": (3, 3, lambda x, a, b: min(max(x, a), b)),
    "lerp": (3, 3, lambda a, b, u: a + (b - a) * u), "step": (2, 2, lambda e, x: 1.0 if x >= e else 0.0),
    "smooth": (1, 1, lambda u: (lambda v: 3 * v * v - 2 * v ** 3)(min(1.0, max(0.0, u)))),
}
# 変数(時刻・seed)を使う関数: 引数の数と、(ctx, *引数) を受ける関数
CTX_FUNCS = {
    "wiggle": (2, 4, lambda c, f, a, o=1, m=0.5: wiggle(c["seed"], c["t"], f, a, o, m)),
    "noise": (1, 1, lambda c, x: _noise1(c["seed"], x)),
    "loop": (1, 1, lambda c, p: c["t"] % p if p > 0 else c["t"]),
    "rand": (0, 2, lambda c, a=None, b=None: (lambda r: r if a is None else (r * a if b is None else a + (b - a) * r))(
        random.Random(c["seed"] * 31 + int(c.get("frame", 0))).random())),
}
VARS = ("t", "dur", "value", "fps", "frame", "pi", "e")


class _P:
    def __init__(self, toks, src, names=()):
        self.t, self.i, self.src, self.names = toks, 0, src, names

    def peek(self):
        return self.t[self.i]

    def take(self, kind=None, val=None):
        k, v = self.t[self.i]
        if (kind and k != kind) or (val is not None and v != val):
            raise ExprError(f"式の書き方が違います({val or kind} が必要): {self.src!r}")
        self.i += 1
        return v

    # 優先順位: 比較 < 加減 < 乗除 < 単項 < べき
    def cmp(self):
        a = self.add()
        while self.peek() in (("o", "<"), ("o", ">"), ("o", "<="), ("o", ">="), ("o", "=="), ("o", "!=")):
            op = self.take()
            b = self.add()
            f = {"<": lambda x, y: x < y, ">": lambda x, y: x > y, "<=": lambda x, y: x <= y, ">=": lambda x, y: x >= y,
                 "==": lambda x, y: x == y, "!=": lambda x, y: x != y}[op]
            a = (lambda f, a, b: lambda c: 1.0 if f(a(c), b(c)) else 0.0)(f, a, b)
        return a

    def add(self):
        a = self.mul()
        while self.peek() in (("o", "+"), ("o", "-")):
            op = self.take()
            b = self.mul()
            a = (lambda a, b: lambda c: a(c) + b(c))(a, b) if op == "+" else (lambda a, b: lambda c: a(c) - b(c))(a, b)
        return a

    def mul(self):
        a = self.unary()
        while self.peek() in (("o", "*"), ("o", "/"), ("o", "%")):
            op = self.take()
            b = self.unary()
            if op == "*":
                a = (lambda a, b: lambda c: a(c) * b(c))(a, b)
            elif op == "/":
                a = (lambda a, b: lambda c: (lambda y: a(c) / y if y else 0.0)(b(c)))(a, b)
            else:
                a = (lambda a, b: lambda c: (lambda y: a(c) % y if y else 0.0)(b(c)))(a, b)
        return a

    def unary(self):
        if self.peek() == ("o", "-"):
            self.take()
            a = self.unary()
            return lambda c: -a(c)
        if self.peek() == ("o", "+"):
            self.take()
            return self.unary()
        return self.power()

    def power(self):
        a = self.atom()
        if self.peek() == ("o", "^"):
            self.take()
            b = self.unary()
            return lambda c: math.pow(a(c), b(c)) if a(c) >= 0 or float(b(c)).is_integer() else 0.0
        return a

    def atom(self):
        k, v = self.peek()
        if k == "n":
            self.take()
            return lambda c, v=v: v
        if k == "o" and v == "(":
            self.take()
            a = self.cmp()
            self.take("o", ")")
            return a
        if k == "v":
            self.take()
            if self.peek() == ("o", "("):
                self.take()
                args = []
                if self.peek() != ("o", ")"):
                    args.append(self.cmp())
                    while self.peek() == ("o", ","):
                        self.take()
                        args.append(self.cmp())
                self.take("o", ")")
                if v in FUNCS:
                    lo, hi, f = FUNCS[v]
                    if not lo <= len(args) <= hi:
                        raise ExprError(f"{v}() の引数の数が違います({lo}〜{hi}個): {self.src!r}")
                    return lambda c, f=f, args=args: float(f(*[a(c) for a in args]))
                if v in CTX_FUNCS:
                    lo, hi, f = CTX_FUNCS[v]
                    if not lo <= len(args) <= hi:
                        raise ExprError(f"{v}() の引数の数が違います({lo}〜{hi}個): {self.src!r}")
                    return lambda c, f=f, args=args: float(f(c, *[a(c) for a in args]))
                raise ExprError(f"知らない関数です: {v}()(使えるもの: {', '.join(sorted(FUNCS) + sorted(CTX_FUNCS))})")
            if v == "pi":
                return lambda c: math.pi
            if v == "e":
                return lambda c: math.e
            if v not in VARS and v not in self.names and not v.startswith("p_"):
                raise ExprError(f"知らない変数です: {v}(使えるもの: {', '.join(VARS + tuple(self.names))})")
            return lambda c, v=v: float(c.get(v, 0.0))
        raise ExprError(f"式の書き方が違います: {self.src!r}")


_CACHE = {}


def compile_expr(src, names=()):
    """式 → fn(ctx) → float。ctx は {"t","dur","value","fps","frame","seed"}。names = ほかに使える変数の名前(プリセットの params)"""
    key = (src, tuple(sorted(names)))
    if key not in _CACHE:
        p = _P(_lex(src), src, key[1])
        f = p.cmp()
        if p.peek()[0] != "end":
            raise ExprError(f"式の終わりに余分なものがあります: {src!r}")
        _CACHE[key] = f
    return _CACHE[key]


def evaluate(src, **ctx):
    ctx.setdefault("seed", 0)
    return compile_expr(src)(ctx)


def evaluate_vars(src, **ctx):
    """ctx の名前をすべて変数として使える evaluate(プリセットの "$=式")"""
    ctx.setdefault("seed", 0)
    return compile_expr(src, tuple(ctx))(ctx)
