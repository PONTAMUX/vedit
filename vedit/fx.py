"""エフェクト・出入りの動き・トランジション・生成要素(集中線・ズーン・フラッシュ)。
動くものはここで1コマずつ描き(numpy / PIL)、透明つき動画(qtrle の .mov)にして proj.cache/fx/ にキャッシュする。
書き出し・vedit frame・GUI のプレビューは、どれもそのキャッシュを使う(描き方はここにしか書かない)。
内容(JSON・素材の更新時刻・解像度・fps)のハッシュでキャッシュするので、変わった要素だけ描き直す。"""
import hashlib
import json
import math
import os
import random
import subprocess
from pathlib import Path

from . import ff, presets
from .timeutil import parse_time

VERSION = 1  # 描き方を変えたら上げる(古いキャッシュを使わない)
STATIC_MIN = 0.3  # 絵が変わらない区間がこれ以上続けば、動画にせず1枚の PNG にする(秒)
TOP_TRACK = 999  # トランジションは一番上に重ねる
TEL_TRACK = 900  # 焼いたテロップ(差し込みの上・トランジションの下)

# ---------------------------------------------------------------- 登録
# 名前 → {"fn", "doc", "params"(既定値)}。vedit fx list と validate と AGENTS.md の表はここから作る
REG = {"anim": {}, "fx": {}, "gen": {}, "trans": {}, "text": {}}


def reg(cat, name, doc, **defaults):
    def deco(fn):
        REG[cat][name] = {"fn": fn, "doc": doc, "params": defaults}
        return fn
    return deco


def params(cat, name, user):
    """既定値に上書き。user は dict(要素の params / fx の各キー)。"""
    return {**REG[cat][name]["params"], **{k: v for k, v in (user or {}).items() if k != "type"}}


def color(c, default=(255, 255, 255)):
    if c is None:
        return default
    if isinstance(c, str):
        s = c.lstrip("#")
        return tuple(int(s[i:i + 2], 16) for i in (0, 2, 4))
    return tuple(int(x) for x in c[:3])


def pair(v):
    return (float(v[0]), float(v[1])) if isinstance(v, (list, tuple)) else (float(v), float(v))


def smooth(u):
    u = min(1.0, max(0.0, u))
    return 3 * u * u - 2 * u ** 3


def _back(u, s=1.70158):
    u -= 1
    return 1 + (s + 1) * u ** 3 + s * u ** 2


def _elastic(u):
    if u <= 0 or u >= 1:
        return min(1.0, max(0.0, u))
    return 2 ** (-10 * u) * math.sin((u * 10 - 0.75) * 2 * math.pi / 3) + 1


def _bounce(u):
    n, d = 7.5625, 2.75
    if u < 1 / d:
        return n * u * u
    if u < 2 / d:
        u -= 1.5 / d
        return n * u * u + 0.75
    if u < 2.5 / d:
        u -= 2.25 / d
        return n * u * u + 0.9375
    u -= 2.625 / d
    return n * u * u + 0.984375


EASE = {"linear": lambda u: u, "smoothstep": smooth,
        "ease-in": lambda u: u * u, "ease-out": lambda u: 1 - (1 - u) ** 2, "ease-in-out": smooth,
        "back": _back, "elastic": _elastic, "bounce": _bounce, "hold": lambda u: 1.0 if u >= 1 else 0.0}


def _bezier(a, b, c, d):
    """cubic-bezier(a,b,c,d)(CSS と同じ。x から t を二分法で求めて y を返す)"""
    def f(u):
        if u <= 0 or u >= 1:
            return min(1.0, max(0.0, u))
        lo, hi = 0.0, 1.0
        for _ in range(40):
            t = (lo + hi) / 2
            x = 3 * a * t * (1 - t) ** 2 + 3 * c * t * t * (1 - t) + t ** 3
            lo, hi = (t, hi) if x < u else (lo, t)
        t = (lo + hi) / 2
        return 3 * b * t * (1 - t) ** 2 + 3 * d * t * t * (1 - t) + t ** 3
    return f


def ease_fn(name):
    """イージングの名前 → 関数。'cubic-bezier(a,b,c,d)' も使える。知らない名前は ValueError"""
    if name in EASE:
        return EASE[name]
    s = str(name).replace(" ", "")
    if s.startswith("cubic-bezier(") and s.endswith(")"):
        v = [float(x) for x in s[13:-1].split(",")]
        if len(v) == 4:
            return _bezier(*v)
    raise ValueError(f"知らないイージングです: {name!r}(使えるもの: {', '.join(EASE)}, cubic-bezier(a,b,c,d))")


def ease(name, u):
    return ease_fn(name)(min(1.0, max(0.0, u)))


# ---------------------------------------------------------------- キーフレーム・式(値を時間で変える)
# 値の代わりに {"keys": [[秒, 値, イージング?], ...], "loop": "cycle|pingpong"?} か {"expr": "式", "value": 元の値?} か "=式" を書ける。
# 秒は要素の頭から。区間 i→i+1 のイージングは「行き先のキーに書いたもの」→無ければ「出発のキーに書いたもの」→無ければ linear
def _lerp_val(a, b, u):
    if isinstance(a, str) and isinstance(b, str) and a.startswith("#") and b.startswith("#"):
        ca, cb = color(a), color(b)
        return "#" + "".join(f"{round(x + (y - x) * u):02X}" for x, y in zip(ca, cb))
    if isinstance(a, (list, tuple)) and isinstance(b, (list, tuple)) and len(a) == len(b):
        return [_lerp_val(x, y, u) for x, y in zip(a, b)]
    if isinstance(a, (int, float)) and isinstance(b, (int, float)) and not isinstance(a, bool):
        return a + (b - a) * u
    return b if u >= 1 else a  # 数でないものは切り替えだけ


def key_value(keys, t, loop=None):
    """キーの並び [[秒, 値, イージング?], ...] の t 秒の値"""
    ks = sorted(keys, key=lambda k: float(k[0]))
    t0, t1 = float(ks[0][0]), float(ks[-1][0])
    if loop and t1 > t0 and t > t1:
        L = t1 - t0
        n, r = divmod(t - t0, L)
        t = t0 + (L - r if loop == "pingpong" and int(n) % 2 else r)
    if t <= t0:
        return ks[0][1]
    if t >= t1:
        return ks[-1][1]
    for i in range(len(ks) - 1):
        a, b = ks[i], ks[i + 1]
        if float(a[0]) <= t <= float(b[0]):
            e = b[2] if len(b) > 2 and b[2] else (a[2] if len(a) > 2 and a[2] else "linear")
            span = float(b[0]) - float(a[0])
            u = ease(e, (t - float(a[0])) / span) if span > 0 else 1.0
            return _lerp_val(a[1], b[1], u)
    return ks[-1][1]


def is_dyn(v):
    return (isinstance(v, dict) and ("keys" in v or "expr" in v)) or (isinstance(v, str) and v.startswith("="))


def pval(v, tau, E, seed=0, base=None):
    """値(キーフレーム・式も可)の τ 秒(要素の頭から)の値。ふつうの dict は中まで見る"""
    if isinstance(v, str) and v.startswith("="):
        v = {"expr": v[1:]}
    if isinstance(v, dict):
        if "keys" in v:
            return key_value(v["keys"], tau, v.get("loop"))
        if "expr" in v:
            from . import expr
            return expr.evaluate(v["expr"], t=tau, dur=(E.b - E.a) if E else 0.0, value=float(v.get("value", base or 0) or 0),
                                 fps=E.fps if E else 30.0, frame=round(tau * (E.fps if E else 30.0)), seed=seed)
        return {k: pval(x, tau, E, seed + i, base) for i, (k, x) in enumerate(v.items())}
    if isinstance(v, list) and any(isinstance(x, dict) for x in v):
        return [pval(x, tau, E, seed + i, base) for i, x in enumerate(v)]
    return v


def has_dyn(v):
    if is_dyn(v):
        return True
    if isinstance(v, dict):
        return any(has_dyn(x) for x in v.values())
    if isinstance(v, list):
        return any(has_dyn(x) for x in v)
    return False


def _seed_of(name, extra=0):
    import zlib
    return zlib.crc32(str(name).encode()) % 100000 + int(extra or 0) * 7


# 要素の keys に書ける動き(値の意味): x/y = 置いた位置からのずれ(1080p 基準 px)、scale/scale_x/scale_y = 倍率(1=原寸)、rot = 角度(度・時計回り)、
# opacity = 不透明度(0〜1、掛け算)、anchor_x/anchor_y = 拡大・回転の中心(中身の外枠に対する比。0.5=真ん中)
KEY_PROPS = ("x", "y", "scale", "scale_x", "scale_y", "rot", "opacity", "anchor_x", "anchor_y")


# ---------------------------------------------------------------- 状態(1コマごとの動き)
class St:
    """要素の動き(1コマぶん)。scale・rot は anchor(ax, ay = 中身の外枠に対する比。既定は真ん中)を中心に、dx/dy は 1080p 基準ではなく実 px。
    blur = ぼかしの半径(実 px)、clip = 見せる範囲 (左, 上, 右, 下)(中身の外枠に対する比。1 を超えてよい)、feather = clip の境目のぼかし(実 px)"""
    __slots__ = ("sx", "sy", "rot", "dx", "dy", "op", "blur", "clip", "feather", "ax", "ay")
    CLIP0 = (-9.0, -9.0, 10.0, 10.0)  # 切らない(境目のぼかしの外まで広い)

    def __init__(self):
        self.sx = self.sy = self.op = 1.0
        self.rot = self.dx = self.dy = self.blur = self.feather = 0.0
        self.ax = self.ay = 0.5
        self.clip = St.CLIP0

    def sig(self):
        return (round(self.sx, 5), round(self.sy, 5), round(self.rot, 4), round(self.dx), round(self.dy), round(self.op, 4),
                round(self.blur, 2), tuple(round(c, 4) for c in self.clip), round(self.feather, 1), round(self.ax, 4), round(self.ay, 4))

    def identity(self):
        return self.sig()[:9] == (1.0, 1.0, 0.0, 0, 0, 1.0, 0.0, St.CLIP0, round(self.feather, 1))

    def clip_to(self, l=None, t=None, r=None, b=None):
        c = self.clip
        self.clip = (max(c[0], l) if l is not None else c[0], max(c[1], t) if t is not None else c[1],
                     min(c[2], r) if r is not None else c[2], min(c[3], b) if b is not None else c[3])


# ---------------------------------------------------------------- 出入りの動き(in_anim / out_anim)
# fn(st, τ, dur, P, E): τ = 出始めてからの秒(out は消えるまでの秒)。τ < dur の時だけ呼ばれる
@reg("anim", "fade", "透明から出る(out は透明へ消える)", dur=0.3)
def _a_fade(st, tau, dur, P, E):
    st.op *= tau / dur


def _slide(axis, sign):
    def fn(st, tau, dur, P, E):
        p = 1 - tau / dur  # render.py の slide と同じ式(画面の外 → 置いた位置)
        x, y, w, h = E.rect
        if axis == "x":
            st.dx += -p * (x + w) if sign < 0 else p * (E.W - x)
        else:
            st.dy += p * (E.H - y) if sign > 0 else -p * (y + h)
    return fn


for _n, _ax, _sg, _d in (("slide-left", "x", -1, "左から入る(out は左へ出る)"), ("slide-right", "x", 1, "右から入る(out は右へ出る)"),
                         ("slide-up", "y", 1, "下から入る(out は下へ出る)"), ("slide-down", "y", -1, "上から入る(out は上へ出る)")):
    reg("anim", _n, _d, dur=0.3)(_slide(_ax, _sg))

POP_SCALE = [1.9, 1.55, 1.28, 1.1, 0.95, 0.98, 1.02, 1.0]
POP_AMP = [0, 0, 0, 0, 14, 12, 10, 8, 6, 4, 2, 0]


@reg("anim", "pop", "大きく出て縮む→少し揺れて止まる(強調テロップの出だし)。dur を12等分した1コマずつ scales の大きさ、"
     "amps の幅(1080p 基準 px)で揺れる。最初の2コマは半透明", dur=0.4, scales=POP_SCALE, amps=POP_AMP, alphas=[0.5, 0.75], seed=0)
def _a_pop(st, tau, dur, P, E):
    step = dur / 12
    k = round(tau / step, 6)
    i = int(k)
    sc = P["scales"]
    s = sc[-1] if i >= len(sc) - 1 else sc[i] + (sc[i + 1] - sc[i]) * (k - i)
    if i >= len(sc):
        s = 1.0
    st.sx *= s
    st.sy *= s
    amps = P["amps"]
    a = (amps[i] if i < len(amps) else 0) * E.k
    if a:
        rnd = E.rand("pop", int(P["seed"]), 2 * len(amps))
        st.dx += rnd[2 * i] * a
        st.dy += rnd[2 * i + 1] * a
    al = P["alphas"]
    if i < len(al):
        st.op *= al[i]


def _zoom(st, tau, dur, P, E):
    u = ease(P["ease"], tau / dur)
    f = float(P["from"])
    s = f + (1 - f) * u
    st.sx *= s
    st.sy *= s
    if P["fade"]:
        st.op *= min(1.0, tau / dur)


reg("anim", "zoom-in", "小さく(from 倍)から拡大して出る。fade=透明から、ease", dur=0.3, **{"from": 0.3}, ease="ease-out", fade=True)(_zoom)
reg("anim", "zoom-out", "大きく(from 倍)から縮んで出る。fade=透明から、ease", dur=0.3, **{"from": 2.0}, ease="ease-out", fade=True)(_zoom)


@reg("anim", "bounce", "上から落ちてきて弾んで止まる。height=落ちる高さ(1080p 基準 px)、fade=最初の1/5で透明から", dur=0.6, height=220, fade=True)
def _a_bounce(st, tau, dur, P, E):
    u = tau / dur
    st.dy -= (1 - _bounce(u)) * float(P["height"]) * E.k
    if P["fade"]:
        st.op *= min(1.0, u * 5)


@reg("anim", "stamp", "スタンプ: 大きく(from 倍)から hit(dur に対する比)で原寸にドンと当たり、残りの時間で小さく揺れて止まる。amp=揺れ幅(1080p 基準 px)",
     dur=0.45, **{"from": 2.4}, hit=0.35, amp=10, seed=0)
def _a_stamp(st, tau, dur, P, E):
    th = dur * float(P["hit"])
    if tau < th:
        u = tau / th
        s = float(P["from"]) + (1 - float(P["from"])) * u * u
        st.sx *= s
        st.sy *= s
        st.op *= min(1.0, u * 1.5)
        return
    rest = max(1e-6, dur - th)
    a = float(P["amp"]) * E.k * (1 - (tau - th) / rest)
    i = int(round((tau - th) * E.fps))
    rnd = E.rand("stamp", int(P["seed"]), 128)
    st.dx += rnd[(2 * i) % 128] * a
    st.dy += rnd[(2 * i + 1) % 128] * a


@reg("anim", "blur-in", "ぼかしから出る(透明から)。radius=最初のぼかし(1080p 基準 px)", dur=0.3, radius=30, ease="ease-out")
def _a_blur(st, tau, dur, P, E):
    u = ease(P["ease"], tau / dur)
    st.blur += float(P["radius"]) * E.k * (1 - u)
    st.op *= u


reg("anim", "blur-out", "ぼけながら消える(out_anim 用。in_anim に書くと blur-in と同じ)。radius=最後のぼかし(1080p 基準 px)",
    dur=0.3, radius=30, ease="ease-out")(_a_blur)
REG["anim"]["blur-out"]["side"] = "out"


@reg("anim", "rotate-in", "回りながら拡大して出る。angle=最初の角度(度。負=反時計回りから)、from=最初の大きさ", dur=0.4, angle=-180, **{"from": 0.2},
     ease="ease-out")
def _a_rotate(st, tau, dur, P, E):
    u = ease(P["ease"], tau / dur)
    st.rot += float(P["angle"]) * (1 - u)
    f = float(P["from"])
    st.sx *= f + (1 - f) * u
    st.sy *= f + (1 - f) * u
    st.op *= min(1.0, 2 * tau / dur)


def _wipe(edge):
    def fn(st, tau, dur, P, E):
        u = ease(P["ease"], tau / dur)
        bb = E.anchor()[2]
        fe = float(P["feather"]) * E.k
        st.feather = max(st.feather, fe)
        w, h = max(1, bb[2] - bb[0]), max(1, bb[3] - bb[1])
        if edge == "left":
            st.clip_to(r=u * (1 + fe / w))
        elif edge == "right":
            st.clip_to(l=1 - u * (1 + fe / w))
        elif edge == "down":
            st.clip_to(b=u * (1 + fe / h))
        else:
            st.clip_to(t=1 - u * (1 + fe / h))
    return fn


for _n, _e, _d in (("wipe-left", "left", "左から右へ現れる"), ("wipe-right", "right", "右から左へ現れる"),
                   ("wipe-up", "up", "下から上へ現れる"), ("wipe-down", "down", "上から下へ現れる")):
    reg("anim", _n, f"マスクで{_d}(out は同じ向きに消える)。feather=境目のぼかし(1080p 基準 px)", dur=0.3, feather=30, ease="ease-out")(_wipe(_e))


@reg("anim", "shake-in", "強く揺れながら出て、揺れがおさまる。amp=[横,縦](1080p 基準 px)、freq=[横,縦]、fade=透明から出る秒", dur=0.4,
     amp=[30, 18], freq=[70, 55], fade=0.1)
def _a_shake_in(st, tau, dur, P, E):
    dx, dy = shake_xy(tau, {"amp": P["amp"], "freq": P["freq"], "dur": dur, "decay": "linear"}, E.k)
    st.dx += dx
    st.dy += dy
    if float(P["fade"]) > 0:
        st.op *= min(1.0, tau / float(P["fade"]))


@reg("anim", "typewriter", "テロップを1文字ずつ出す(telops 専用。out は1文字ずつ消える)。interval=1文字の秒(dur は 文字数×interval になる)",
     dur=0, interval=0.06)
def _a_typewriter(st, tau, dur, P, E):
    pass  # 中身(どこまで見せるか)は Elem.content で決める


REG["anim"]["typewriter"]["telop_only"] = True


# ---------------------------------------------------------------- エフェクト(fx)
# kind="state": 動き(St)を変える / "pixel": 描いた絵を変える。start/end は要素の頭からの秒(省略=全体)
def shake_xy(tau, P, k=1.0):
    """揺れの量(px)。だんだん弱まる sin/cos。V1 の揺れ(ffmpeg の式 shake_expr)と同じ式"""
    d = float(P["dur"])
    if tau < 0 or tau >= d:
        return 0.0, 0.0
    dec = {"linear": 1 - tau / d, "exp": math.exp(-4 * tau / d), "none": 1.0}[P.get("decay", "linear")]
    (ax, ay), (fx_, fy) = pair(P["amp"]), pair(P["freq"])
    return ax * k * math.sin(tau * fx_) * dec, ay * k * math.cos(tau * fy) * dec


def shake_expr(P, t0, k=1.0):
    """V1(背景)の揺れの ffmpeg 式(変数 t)。shake_xy と同じ式"""
    d = float(P["dur"])
    T = f"(t-{t0:.4f})"
    dec = {"linear": f"(1-{T}/{d})", "exp": f"exp(-4*{T}/{d})", "none": "1"}[P.get("decay", "linear")]
    (ax, ay), (fx_, fy) = pair(P["amp"]), pair(P["freq"])
    return f"{ax * k:.3f}*sin({T}*{fx_})*{dec}", f"{ay * k:.3f}*cos({T}*{fy})*{dec}"


@reg("fx", "shake", "揺れ(だんだん弱まる sin)。amp=[横,縦] 1080p 基準 px、freq=[横,縦] 角速度(rad/秒)、dur=揺れる秒、"
     "decay=linear|exp|none。overlays の fx、v1_fx(背景)に使える", amp=[12, 7], freq=[95, 77], dur=0.5, decay="linear")
def _f_shake(st, tau, P, E):
    dx, dy = shake_xy(tau, P, E.k)
    st.dx += dx
    st.dy += dy


REG["fx"]["shake"]["kind"] = "state"


@reg("fx", "zoom", "ゆっくり寄る/引く(Ken Burns)。from→to の倍率へ dur 秒(省略=fx の終わりまで)で、center=[x,y](中身の外枠に対する比)を中心に。ease",
     **{"from": 1.0, "to": 1.15}, center=[0.5, 0.5], dur=None, ease="linear")
def _f_zoom(st, tau, P, E):
    d = float(P["dur"] or P.get("_len") or (E.b - E.a))
    u = ease(P["ease"], tau / d) if d > 0 else 1.0
    s = float(P["from"]) + (float(P["to"]) - float(P["from"])) * u
    st.sx *= s
    st.sy *= s
    st.ax, st.ay = pair(P["center"])


@reg("fx", "drift", "ゆっくり流れる。to=[横,縦] 動く量(1080p 基準 px)を dur 秒(省略=fx の終わりまで)で。ease", to=[60, 0], dur=None, ease="linear")
def _f_drift(st, tau, P, E):
    d = float(P["dur"] or P.get("_len") or (E.b - E.a))
    u = ease(P["ease"], tau / d) if d > 0 else 1.0
    tx, ty = pair(P["to"])
    st.dx += tx * E.k * u
    st.dy += ty * E.k * u


for _n in ("zoom", "drift"):
    REG["fx"][_n]["kind"] = "state"


# --- 絵を変えるエフェクト(kind="pixel"): fn(c, τ, P, E) → c。c は画面全体の RGBA uint8(色は透明度で割っていない)
# どれも region(下の shape_mask と同じ書き方)を付けると、その範囲だけにかかる。mosaic / blur-region は rect 等を直接書ける
def shape_mask(E, spec):
    """範囲の形 → (H,W) float32 の 0〜1。spec: {"shape": "rect|ellipse|polygon", "rect": [x,y,w,h], "points": [[x,y],...],
    "feather": 境目のぼかし, "invert": 外側}。座標は 1080p 基準 px(画面の左上が 0,0)"""
    import numpy as np
    import cv2
    key = ("mask", json.dumps(spec, sort_keys=True, default=str))
    m = E._memo.get(key)
    if m is not None:
        return m
    k, W, H = E.k, E.W, E.H
    m = np.zeros((H, W), np.uint8)
    shape = spec.get("shape") or ("rect" if spec.get("rect") else "polygon")
    if shape in ("rect", "ellipse"):
        x, y, w, h = [float(v) * k for v in (spec.get("rect") or [0, 0, 1920, 1080])]
        if shape == "rect":
            cv2.rectangle(m, (round(x), round(y)), (round(x + w) - 1, round(y + h) - 1), 255, -1)
        else:
            cv2.ellipse(m, (round(x + w / 2), round(y + h / 2)), (max(1, round(w / 2)), max(1, round(h / 2))), 0, 0, 360, 255, -1, cv2.LINE_AA)
    else:
        pts = np.array([[float(px) * k, float(py) * k] for px, py in spec.get("points") or []], np.float32)
        if len(pts) >= 3:
            cv2.fillPoly(m, [np.round(pts * 16).astype(np.int32)], 255, cv2.LINE_AA, 4)
    f = m.astype(np.float32) / 255
    fe = float(spec.get("feather", 0) or 0) * k
    if fe > 0.3:
        f = cv2.GaussianBlur(f, (0, 0), fe / 2)
    if spec.get("invert"):
        f = 1 - f
    if len(E._memo) < 400:
        E._memo[key] = f
    return f


def _region_of(P):
    if isinstance(P.get("region"), dict):
        return P["region"]
    if P.get("rect") or P.get("points"):
        return {"shape": P.get("shape") or ("rect" if P.get("rect") else "polygon"), "rect": P.get("rect"), "points": P.get("points"),
                "feather": P.get("feather", 0), "invert": P.get("invert", False)}
    return None


def _mixm(c, r, m):
    """c と r を m(0〜1)の割合で混ぜる"""
    import numpy as np
    if m is None:
        return r
    out = c.astype(np.float32) + (r.astype(np.float32) - c.astype(np.float32)) * m[..., None]
    return np.clip(out + 0.5, 0, 255).astype(np.uint8)


def _pm(c):
    """透明度を掛けた float32(0〜255)"""
    import numpy as np
    f = c.astype(np.float32)
    f[..., :3] *= f[..., 3:4] / 255
    return f


def _unpm(f):
    import numpy as np
    a = f[..., 3:4]
    out = np.empty(f.shape, np.uint8)
    out[..., :3] = np.clip(np.where(a > 0.5, f[..., :3] * 255 / np.maximum(a, 1e-6), 0) + 0.5, 0, 255)
    out[..., 3] = np.clip(f[..., 3] + 0.5, 0, 255)
    return out


def _gauss(c, r):
    import cv2
    if r < 0.3:
        return c
    return _unpm(cv2.GaussianBlur(_pm(c), (0, 0), r))


def _rgb_op(c, fn):
    """色だけ変える(透明度はそのまま)。fn(rgb float32 0〜1) → rgb"""
    import numpy as np
    out = c.copy()
    out[..., :3] = np.clip(fn(c[..., :3].astype(np.float32) / 255) * 255 + 0.5, 0, 255).astype(np.uint8)
    return out


def _luma(rgb):
    return rgb[..., 0] * 0.2126 + rgb[..., 1] * 0.7152 + rgb[..., 2] * 0.0722


def _px(name, doc, **d):
    def deco(fn):
        reg("fx", name, doc + "。region={shape,rect,...} を付けるとその範囲だけ", **d)(fn)
        REG["fx"][name]["kind"] = "pixel"
        return fn
    return deco


@_px("mosaic", "モザイク(プレイヤー名を隠すなど)。rect=[x,y,w,h](1080p 基準 px。省略=全体)、shape=rect|ellipse|polygon(points)、size=ブロックの大きさ(px)、"
     "feather=境目のぼかし。rect/points はキーフレームで動かせる", rect=None, shape="rect", points=None, size=24, feather=0, invert=False)
def _x_mosaic(c, t, P, E):
    import cv2
    b = max(2, round(float(P["size"]) * E.k))
    H, W = c.shape[:2]
    f = _pm(c)
    small = cv2.resize(f, (max(1, W // b), max(1, H // b)), interpolation=cv2.INTER_AREA)
    return _unpm(cv2.resize(small, (W, H), interpolation=cv2.INTER_NEAREST))


@_px("blur-region", "範囲をぼかす(名前を隠すなど)。rect=[x,y,w,h](1080p 基準 px)、shape、points、radius=ぼかし(px)、feather。rect/points はキーフレームで動かせる",
     rect=[760, 440, 400, 200], shape="rect", points=None, radius=20, feather=8, invert=False)
def _x_blur_region(c, t, P, E):
    return _gauss(c, float(P["radius"]) * E.k)


@_px("blur", "全体をぼかす。radius=ぼかし(1080p 基準 px)", radius=8)
def _x_blur(c, t, P, E):
    return _gauss(c, float(P["radius"]) * E.k)


@_px("motion-blur", "動きのぶれ。length=0 なら要素自身の動き(出入り・keys・揺れ)から作る(shutter=1コマのうち開いている割合、samples=重ねる数)。"
     "length>0 なら angle(度)の向きに length(1080p 基準 px)ぶれる", length=0, angle=0, shutter=0.5, samples=8)
def _x_motion_blur(c, t, P, E):
    import numpy as np
    import cv2
    n = max(2, int(P["samples"]))
    L = float(P["length"]) * E.k
    if L > 0.5:  # 決まった向きにぶらす
        a = math.radians(float(P["angle"]))
        f = _pm(c)
        acc = np.zeros_like(f)
        for i in range(n):
            d = (i / (n - 1) - 0.5) * L
            M = np.float32([[1, 0, d * math.cos(a)], [0, 1, d * math.sin(a)]])
            acc += cv2.warpAffine(f, M, (f.shape[1], f.shape[0]), borderMode=cv2.BORDER_CONSTANT)
        return _unpm(acc / n)
    tau = E._tau
    sh = float(P["shutter"]) / E.fps
    f = np.zeros(c.shape, np.float32)
    for i in range(n):
        f += _pm(E.render(max(0.0, tau - sh * i / (n - 1)), no_mb=True) if i else c)
    return _unpm(f / n)


@_px("glow", "光る(明るい所がにじむ)。radius=にじむ幅(1080p 基準 px)、strength=強さ、threshold=光らせる明るさ(0〜1)、color=光の色(null=元の色)",
     radius=20, strength=1.0, threshold=0.6, color=None)
def _x_glow(c, t, P, E):
    import numpy as np
    import cv2
    f = c.astype(np.float32) / 255
    rgb, a = f[..., :3], f[..., 3:4]
    th = float(P["threshold"])
    w = np.clip((_luma(rgb) - th) / max(1e-3, 1 - th), 0, 1)[..., None] * a
    src = (np.array(color(P["color"]), np.float32) / 255 if P["color"] else rgb) * w
    r = float(P["radius"]) * E.k
    g = cv2.GaussianBlur(np.concatenate([src, w], 2), (0, 0), max(0.5, r / 2)) * float(P["strength"])
    gl, ga = g[..., :3], np.clip(g[..., 3:4], 0, 1)
    out_a = np.clip(a + ga * (1 - a), 0, 1)
    pm = rgb * a
    pm = 1 - (1 - pm) * (1 - np.clip(gl, 0, 1))  # screen で足す
    res = np.where(out_a > 1e-4, pm / np.maximum(out_a, 1e-4), 0)
    return np.clip(np.concatenate([res, out_a], 2) * 255 + 0.5, 0, 255).astype(np.uint8)


@_px("shadow", "影(要素の形の影を下に置く)。x,y=ずらす量(1080p 基準 px)、blur=影のぼかし、color、opacity", x=8, y=8, blur=8, color="#000000", opacity=0.6)
def _x_shadow(c, t, P, E):
    import numpy as np
    import cv2
    a = c[..., 3].astype(np.float32) / 255
    M = np.float32([[1, 0, float(P["x"]) * E.k], [0, 1, float(P["y"]) * E.k]])
    sa = cv2.warpAffine(a, M, (a.shape[1], a.shape[0]))
    if float(P["blur"]) > 0:
        sa = cv2.GaussianBlur(sa, (0, 0), float(P["blur"]) * E.k / 2)
    sa *= float(P["opacity"])
    sh = np.zeros(c.shape, np.uint8)
    sh[..., :3] = color(P["color"], (0, 0, 0))
    sh[..., 3] = np.clip(sa * 255 + 0.5, 0, 255).astype(np.uint8)
    return over(sh, c)


@_px("brightness", "明るさ。amount=-1〜1(0=そのまま)", amount=0.2)
def _x_brightness(c, t, P, E):
    return _rgb_op(c, lambda x: x + float(P["amount"]))


@_px("contrast", "コントラスト。amount=-1〜1(0=そのまま)", amount=0.2)
def _x_contrast(c, t, P, E):
    return _rgb_op(c, lambda x: (x - 0.5) * (1 + float(P["amount"])) + 0.5)


@_px("saturation", "彩度。amount=-1〜1(-1=白黒、0=そのまま)", amount=0.3)
def _x_saturation(c, t, P, E):
    return _rgb_op(c, lambda x: _luma(x)[..., None] + (x - _luma(x)[..., None]) * (1 + float(P["amount"])))


@_px("tint", "色をつける(暗い所→black の色、明るい所→white の色)。amount=かかり具合", black="#000000", white="#FFB040", amount=1.0)
def _x_tint(c, t, P, E):
    import numpy as np
    b = np.array(color(P["black"]), np.float32) / 255
    w = np.array(color(P["white"]), np.float32) / 255
    am = float(P["amount"])
    return _rgb_op(c, lambda x: x + ((b + (w - b) * _luma(x)[..., None]) - x) * am)


@_px("grayscale", "白黒。amount=かかり具合", amount=1.0)
def _x_gray(c, t, P, E):
    return _rgb_op(c, lambda x: x + (_luma(x)[..., None] - x) * float(P["amount"]))


@_px("invert", "色を反転。amount=かかり具合", amount=1.0)
def _x_invert(c, t, P, E):
    return _rgb_op(c, lambda x: x + ((1 - x) - x) * float(P["amount"]))


@_px("vignette", "周りを暗く(color に)する。amount=濃さ、radius=暗くし始める所(画面の対角の半分に対する比)、soft=暗くなる幅", amount=0.6, radius=0.6,
     soft=0.5, color="#000000")
def _x_vignette(c, t, P, E):
    import numpy as np

    def mk():
        H, W = c.shape[:2]
        ys, xs = np.mgrid[0:H, 0:W].astype(np.float32)
        d = np.hypot((xs - W / 2) / (W / 2), (ys - H / 2) / (H / 2)) / math.sqrt(2)
        return np.clip((d - float(P["radius"])) / max(1e-3, float(P["soft"])), 0, 1) ** 1.5
    v = E.memo(("vig", P["radius"], P["soft"]), mk) * float(P["amount"])
    col = np.array(color(P["color"], (0, 0, 0)), np.float32) / 255
    return _rgb_op(c, lambda x: x + (col - x) * v[..., None])


@_px("chroma-key", "色を抜く(グリーンバック)。color=抜く色、similarity=抜く近さ(0〜1)、smooth=境目の幅、spill=色かぶりを抑える強さ",
     color="#00FF00", similarity=0.35, smooth=0.1, spill=0.5)
def _x_chroma(c, t, P, E):
    import numpy as np
    f = c.astype(np.float32) / 255
    rgb = f[..., :3]
    kc = np.array(color(P["color"]), np.float32) / 255

    def cbcr(x):
        y = _luma(x)
        return np.stack([(x[..., 2] - y) / 1.8556, (x[..., 0] - y) / 1.5748], -1)
    d = np.linalg.norm(cbcr(rgb) - cbcr(kc[None, None, :]), axis=-1) / 0.75
    a = np.clip((d - float(P["similarity"])) / max(1e-3, float(P["smooth"])), 0, 1)
    out = f.copy()
    out[..., 3] *= a
    ch = int(np.argmax(kc))  # 抜く色のいちばん強い成分のかぶりを抑える
    o1, o2 = [i for i in range(3) if i != ch]
    lim = np.maximum(rgb[..., o1], rgb[..., o2])
    out[..., ch] = rgb[..., ch] - np.maximum(0, rgb[..., ch] - lim) * float(P["spill"])
    return np.clip(out * 255 + 0.5, 0, 255).astype(np.uint8)


@_px("rgb-split", "色ずれ(赤と青を逆向きにずらす)。amount=ずらす量(1080p 基準 px)、angle=向き(度)", amount=8, angle=0)
def _x_rgb_split(c, t, P, E):
    import numpy as np
    import cv2
    d = float(P["amount"]) * E.k
    a = math.radians(float(P["angle"]))
    f = _pm(c)
    H, W = c.shape[:2]
    sh = lambda img, k: cv2.warpAffine(np.ascontiguousarray(img), np.float32([[1, 0, k * d * math.cos(a)], [0, 1, k * d * math.sin(a)]]), (W, H))
    r, b = sh(f[..., [0, 3]], 1), sh(f[..., [2, 3]], -1)
    out = np.empty_like(f)
    out[..., 0], out[..., 1], out[..., 2] = r[..., 0], f[..., 1], b[..., 0]
    out[..., 3] = np.maximum(np.maximum(r[..., 1], b[..., 1]), f[..., 3])
    return _unpm(out)


@_px("glitch", "グリッチ(横の帯がずれて色がずれる)。strength=強さ、every=何コマごとにずれ方を変えるか、seed", strength=1.0, every=2, seed=7)
def _x_glitch(c, t, P, E):
    import numpy as np
    H, W = c.shape[:2]
    g = float(P["strength"])
    fr = int(round(t * E.fps)) // max(1, int(P["every"]))
    rnd = random.Random(int(P["seed"]) * 100003 + fr)
    out = c.copy()
    y = 0
    while y < H:
        hh = rnd.randint(int(H * 0.01) + 1, int(H * 0.1) + 2)
        if rnd.random() < 0.45 * g + 0.05:
            out[y:y + hh] = np.roll(c[y:y + hh], int(rnd.uniform(-1, 1) * g * W * 0.06), axis=1)
        y += hh
    d = int(round(g * 10 * E.k * rnd.uniform(0.3, 1)))
    if d:
        out[..., 0] = np.roll(out[..., 0], d, axis=1)
        out[..., 2] = np.roll(out[..., 2], -d, axis=1)
    return out


@_px("mirror", "反転。axis=x(左右)|y(上下)、half=true で半分を折り返す(左半分を右へ写す)", axis="x", half=False)
def _x_mirror(c, t, P, E):
    import numpy as np
    ax = 1 if P["axis"] == "x" else 0
    fl = np.flip(c, axis=ax)
    if not P["half"]:
        return fl.copy()
    out = c.copy()
    n = c.shape[ax]
    if ax == 1:
        out[:, n // 2:] = fl[:, n // 2:]
    else:
        out[n // 2:] = fl[n // 2:]
    return out


@_px("outline", "縁取り(形のまわりに線)。width=太さ(1080p 基準 px)、color、opacity", width=6, color="#000000", opacity=1.0)
def _x_outline(c, t, P, E):
    import numpy as np
    import cv2
    w = max(1, round(float(P["width"]) * E.k))
    a = c[..., 3]
    ys, xs = np.nonzero(a)
    if not len(xs):
        return c
    x0, y0 = max(0, xs.min() - w - 2), max(0, ys.min() - w - 2)
    x1, y1 = min(c.shape[1], xs.max() + w + 3), min(c.shape[0], ys.max() + w + 3)
    k = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * w + 1, 2 * w + 1))
    da = cv2.dilate(np.ascontiguousarray(a[y0:y1, x0:x1]), k)
    sh = np.zeros(c.shape, np.uint8)
    sh[..., :3] = color(P["color"], (0, 0, 0))
    sh[y0:y1, x0:x1, 3] = (da.astype(np.float32) * float(P["opacity"])).astype(np.uint8)
    return over(sh, c)


REG["fx"]["mosaic"]["region"] = REG["fx"]["blur-region"]["region"] = True  # 範囲だけ変わる(背景にかけた時は変わった所だけ焼く)


def over(dst, src):
    """src を dst の上に重ねる(どちらも RGBA uint8・色は透明度で割っていない)"""
    import numpy as np
    s = src.astype(np.float32) / 255
    d = dst.astype(np.float32) / 255
    sa, da = s[..., 3:4], d[..., 3:4]
    oa = sa + da * (1 - sa)
    rgb = np.where(oa > 1e-5, (s[..., :3] * sa + d[..., :3] * da * (1 - sa)) / np.maximum(oa, 1e-5), 0)
    return np.clip(np.concatenate([rgb, oa], 2) * 255 + 0.5, 0, 255).astype(np.uint8)


def _np_min(b, s):
    import numpy as np
    return np.minimum(b, s)


def _np_max(b, s):
    import numpy as np
    return np.maximum(b, s)


def _overlay_b(b, s):
    import numpy as np
    return np.where(b < 0.5, 2 * b * s, 1 - 2 * (1 - b) * (1 - s))


# 重ね方(要素の blend)。b=下の色、s=上の色(0〜1)
BLEND = {"normal": lambda b, s: s, "add": lambda b, s: (b + s).clip(0, 1), "screen": lambda b, s: 1 - (1 - b) * (1 - s),
         "multiply": lambda b, s: b * s, "overlay": _overlay_b, "darken": _np_min, "lighten": _np_max, "difference": lambda b, s: abs(b - s)}


# ---------------------------------------------------------------- 生成要素(overlays の kind。file なし)
# fn(E, τ, P) → (H,W,4) uint8 の絵。animated=False なら1回だけ描いて使い回す
def _empty(E):
    import numpy as np
    return np.zeros((E.H, E.W, 4), np.uint8)


@reg("gen", "speedlines", "集中線(真ん中は空ける)。count=本数、inner=空ける半径(画面の対角の半分に対する比)、color、seed。"
     "animate=true で every コマごとに線を描き直す(ちらつく)", count=110, inner=0.42, color="#FFFFFF", seed=3, blur=1.2,
     animate=False, every=2)
def _g_speedlines(E, tau, P):
    import numpy as np
    from PIL import Image, ImageDraw, ImageFilter
    seed = int(P["seed"]) + (int(round(tau * E.fps)) // max(1, int(P["every"])) if P["animate"] else 0)
    rnd = random.Random(seed)
    W, H, n, inner = E.W, E.H, int(P["count"]), float(P["inner"])
    col = color(P["color"])
    im = Image.new("RGBA", (W, H))
    d = ImageDraw.Draw(im)
    cx, cy, R = W / 2, H / 2, math.hypot(W, H) / 2 + 20 * E.k
    for i in range(n):
        a = 2 * math.pi * (i + rnd.uniform(-0.4, 0.4)) / n
        r0 = R * rnd.uniform(inner, inner + 0.22)
        wd = rnd.uniform(0.006, 0.016)
        pts = [(cx + R * math.cos(a - wd), cy + R * math.sin(a - wd)), (cx + R * math.cos(a + wd), cy + R * math.sin(a + wd)),
               (cx + r0 * math.cos(a), cy + r0 * math.sin(a))]
        d.polygon(pts, fill=(*col, rnd.randint(170, 240)))
    if float(P["blur"]) > 0:
        im = im.filter(ImageFilter.GaussianBlur(float(P["blur"]) * E.k))
    return np.asarray(im).copy()


REG["gen"]["speedlines"]["animated"] = lambda P: bool(P["animate"])


@reg("gen", "gloom", "ズーン: 画面を暗くして、上から縦線(ガーン線)を垂らす。dark=暗さ(0〜1)、bg=暗くする色、line_color、"
     "line_alpha(0〜255)、seed。grow=秒(0=最初から全部。>0 で線が上から伸びる)", dark=0.5, bg="#08061E", line_color="#968CFF",
     line_alpha=150, seed=5, grow=0)
def _g_gloom(E, tau, P):
    import numpy as np
    from PIL import Image, ImageDraw, ImageFilter
    rnd = random.Random(int(P["seed"]))
    W, H, k = E.W, E.H, E.k
    im = Image.new("RGBA", (W, H), (*color(P["bg"]), int(float(P["dark"]) * 255)))
    ln = Image.new("RGBA", (W, H))
    d = ImageDraw.Draw(ln)
    g = 1.0 if float(P["grow"]) <= 0 else smooth(tau / float(P["grow"]))
    lc = (*color(P["line_color"]), int(P["line_alpha"]))
    x = 20 * k
    while x < W:
        L = rnd.uniform(0.18, 0.42) * H * g
        w = rnd.choice((3, 4, 6)) * k
        if L > 0:
            d.rectangle([x, 0, x + w, L], fill=lc)
        x += rnd.randint(26, 60) * k
    ln = ln.filter(ImageFilter.GaussianBlur(1 * k))
    grad = np.linspace(1, 0, H, dtype=np.float32)[:, None] ** 0.7  # 線は下へ行くほど薄く
    la = np.asarray(ln.getchannel("A"), np.float32) * grad
    ln.putalpha(Image.fromarray(la.astype(np.uint8)))
    im.alpha_composite(ln)
    return np.asarray(im).copy()


REG["gen"]["gloom"]["animated"] = lambda P: float(P["grow"]) > 0


@reg("gen", "flash", "フラッシュ: 色が一瞬出てすぐ消える。dur=全体の秒(end を省くとこの長さ)、fade=最後に消えていく秒、opacity、color",
     color="#FFFFFF", opacity=0.6, dur=0.14, fade=0.1)
def _g_flash(E, tau, P):
    import numpy as np
    d, f = float(P["dur"]), float(P["fade"])
    a = float(P["opacity"]) * (1.0 if tau < d - f else max(0.0, (d - tau) / f) if f > 0 else 0.0)
    img = np.zeros((E.H, E.W, 4), np.uint8)
    img[..., :3] = color(P["color"])
    img[..., 3] = round(a * 255)
    return img


REG["gen"]["flash"]["animated"] = lambda P: True


# ---------------------------------------------------------------- トランジション
# fn(E, tau, P): tau = 継ぎ目からの秒(前は負)。継ぎ目の前 dur 秒で覆い、後 dur 秒で抜ける(全面を覆う絵のトランジション)
@reg("trans", "wipe-diag", "斜めの帯: 赤い先端+黒の帯が左から画面を覆い(継ぎ目の前 dur 秒)、同じ向きに右へ抜ける(後 dur 秒)。"
     "angle=傾き(度)、edge_color/edge_w=先端の色と幅(1080p 基準 px)、color=帯の色、ease", angle=18, edge_color="#C8141B", edge_w=90,
     color="#0E0E12", dur=0.4, ease="smoothstep")
def _t_wipe_diag(E, tau, P):
    import numpy as np
    W, H = E.W, E.H
    k = math.tan(math.radians(float(P["angle"])))
    X = E.memo("wipe-X", lambda: np.arange(W, dtype=np.float32)[None, :] + (H - np.arange(H, dtype=np.float32)[:, None]) * k)
    R = float(P["edge_w"]) * E.k
    L = W + H * k + R
    dur = float(P["dur"])
    ease = EASE[P["ease"]]
    cover = tau < 0
    if cover:  # 最後のコマで全面(元の gfx.wipe と同じく1コマ先の位置)
        u = ease(min(1.0, (tau + dur + 1 / E.fps) / dur))
        edge = u * (L + 2) - 1
        dd = edge - X
    else:
        u = ease(min(1.0, tau / dur))
        edge = u * (L + 2) - R - 1
        dd = X - edge
    alpha = np.clip(dd + 0.5, 0, 1)
    red = np.clip(R - dd + 0.5, 0, 1) * alpha
    ec, bc = color(P["edge_color"]), color(P["color"])
    img = np.empty((H, W, 4), np.float32)
    for c in range(3):
        img[..., c] = ec[c] * red + bc[c] * (1 - red)
    img[..., 3] = 255 * alpha
    img[..., :3] *= (alpha > 0)[..., None]
    return img.clip(0, 255).astype(np.uint8)


def _cover_u(E, tau, P):
    """覆う量 0〜1: 継ぎ目の前 dur 秒で 0→1(継ぎ目の直前のコマで 1)、後 dur 秒で 1→0"""
    dur = float(P["dur"])
    if tau < 0:
        return ease(P["ease"], (tau + dur + 1 / E.fps) / dur)
    return 1 - ease(P["ease"], tau / dur)


def _solid(E, rgb, a):
    import numpy as np
    img = np.zeros((E.H, E.W, 4), np.uint8)
    img[..., :3] = rgb
    img[..., 3] = round(255 * min(1.0, max(0.0, a)))
    return img


@reg("trans", "dip-black", "黒に溶けて、黒から出る(継ぎ目の前後 dur 秒ずつ)", dur=0.15, color="#000000", ease="linear")
def _t_dip(E, tau, P):
    return _solid(E, color(P["color"]), _cover_u(E, tau, P))


reg("trans", "dip-white", "白に溶けて、白から出る(継ぎ目の前後 dur 秒ずつ)", dur=0.15, color="#FFFFFF", ease="linear")(_t_dip)


@reg("trans", "flash", "継ぎ目で白く光る(前後 dur 秒で強まって消える)。opacity=いちばん強い時の濃さ", dur=0.12, color="#FFFFFF", opacity=1.0,
     ease="ease-in")
def _t_flash(E, tau, P):
    return _solid(E, color(P["color"]), float(P["opacity"]) * _cover_u(E, tau, P))


@reg("trans", "iris", "丸く閉じて(継ぎ目で全部 color)、丸く開く。center=[x,y](画面比)、soft=縁のぼかし(1080p 基準 px)", dur=0.35,
     color="#000000", center=[0.5, 0.5], soft=3, ease="ease-in-out")
def _t_iris(E, tau, P):
    import numpy as np
    W, H = E.W, E.H
    cx, cy = float(P["center"][0]) * W, float(P["center"][1]) * H
    D = E.memo(("iris", cx, cy), lambda: np.hypot(np.arange(W, dtype=np.float32)[None, :] - cx, np.arange(H, dtype=np.float32)[:, None] - cy))
    Rm = float(D.max()) + 2
    r = (1 - _cover_u(E, tau, P)) * Rm
    s = max(0.5, float(P["soft"]) * E.k)
    a = np.clip((D - r) / s + 0.5, 0, 1)
    img = np.zeros((H, W, 4), np.uint8)
    img[..., :3] = color(P["color"])
    img[..., 3] = (a * 255).astype(np.uint8)
    return img


# --- 前後の映像を使うトランジション(src)。A = 継ぎ目の前の映像(継ぎ目の後もカットされた続きを使う)、B = 後の映像(前はカットされた手前)
def _mix(E, tau, P):
    """継ぎ目の前後 dur 秒をまとめた進み具合 0〜1"""
    dur = float(P["dur"])
    return ease(P["ease"], (tau + dur) / (2 * dur))


@reg("trans", "crossfade", "前の映像と後の映像を重ねて入れ替える(継ぎ目の前後 dur 秒ずつ。カットされた部分の映像を使う)", dur=0.25, ease="linear")
def _t_crossfade(E, tau, P):
    import numpy as np
    u = _mix(E, tau, P)
    A, B = E.src_frame("A", tau), E.src_frame("B", tau)
    out = A.astype(np.float32) * (1 - u) + B.astype(np.float32) * u
    return E.opaque(out.astype(np.uint8))


def _push(dx, dy):
    def fn(E, tau, P):
        import numpy as np
        u = _mix(E, tau, P)
        A, B = E.src_frame("A", tau), E.src_frame("B", tau)
        W, H = E.W, E.H
        out = np.empty_like(A)
        if dx:
            o = round(u * W)
            if dx < 0:  # 左へ押し出す: A が左へ、B が右から
                out[:, :W - o] = A[:, o:]
                out[:, W - o:] = B[:, :o]
            else:
                out[:, o:] = A[:, :W - o]
                out[:, :o] = B[:, W - o:]
        else:
            o = round(u * H)
            if dy < 0:
                out[:H - o] = A[o:]
                out[H - o:] = B[:o]
            else:
                out[o:] = A[:H - o]
                out[:o] = B[H - o:]
        return E.opaque(out)
    return fn


for _n, _dx, _dy, _d in (("push-left", -1, 0, "左"), ("push-right", 1, 0, "右"), ("push-up", 0, -1, "上"), ("push-down", 0, 1, "下")):
    reg("trans", _n, f"前の映像を{_d}へ押し出して、後の映像が入ってくる(継ぎ目の前後 dur 秒ずつ)", dur=0.3, ease="smoothstep")(_push(_dx, _dy))


def _zoom_img(arr, s, W, H):
    """中心から s 倍に拡大(はみ出しは切る)"""
    from PIL import Image
    import numpy as np
    if s <= 1.0005:
        return arr
    cw, ch = W / s, H / s
    box = ((W - cw) / 2, (H - ch) / 2, (W + cw) / 2, (H + ch) / 2)
    return np.asarray(Image.fromarray(arr).resize((W, H), Image.BILINEAR, box=box))


@reg("trans", "zoom", "前の映像に寄っていき(scale 倍まで)、後の映像は寄った所から引いて出る。blur=寄る勢いのぶれ(0〜)", dur=0.25, scale=1.8,
     blur=0.08)
def _t_zoom(E, tau, P):
    import numpy as np
    dur, S, bl = float(P["dur"]), float(P["scale"]), float(P["blur"])
    if tau < 0:
        u = EASE["ease-in"](min(1.0, (tau + dur + 1 / E.fps) / dur))
        src, s = E.src_frame("A", tau), 1 + (S - 1) * u
    else:
        u = 1 - EASE["ease-out"](min(1.0, tau / dur))
        src, s = E.src_frame("B", tau), 1 + (S - 1) * u
    if bl > 0 and u > 0.05:  # 少しずつ大きさの違う絵を重ねて、寄る勢いのぶれにする
        acc = np.zeros(src.shape, np.float32)
        steps = 4
        for j in range(steps):
            acc += _zoom_img(src, s * (1 + bl * u * j / (steps - 1)), E.W, E.H)
        return E.opaque((acc / steps).astype(np.uint8))
    return E.opaque(_zoom_img(src, s, E.W, E.H))


def _hblur(arr, r):
    """横方向のぶれ(箱ぼかし、端は回り込む)"""
    import numpy as np
    r = int(r)
    if r < 1:
        return arr
    a = arr.astype(np.float32)
    pad = np.concatenate([a[:, -r:], a, a[:, :r + 1]], axis=1)
    cs = np.cumsum(pad, axis=1)
    out = (cs[:, 2 * r + 1:] - cs[:, :-2 * r - 1]) / (2 * r + 1)
    return out[:, :arr.shape[1]].astype(np.uint8)


@reg("trans", "whip", "速い横ブレ(ホイップパン): 前の映像が左へ流れ、後の映像が右から流れて止まる。dist=流れる距離(画面幅に対する比)、blur=ぶれの強さ",
     dur=0.2, dist=0.6, blur=0.12)
def _t_whip(E, tau, P):
    import numpy as np
    dur, W = float(P["dur"]), E.W
    if tau < 0:
        u = EASE["ease-in"](min(1.0, (tau + dur + 1 / E.fps) / dur))
        src, off = E.src_frame("A", tau), -u * float(P["dist"]) * W
    else:
        u = 1 - EASE["ease-out"](min(1.0, tau / dur))
        src, off = E.src_frame("B", tau), u * float(P["dist"]) * W
    out = np.roll(src, int(round(off)), axis=1)
    return E.opaque(_hblur(out, u * float(P["blur"]) * W))


@reg("trans", "glitch", "継ぎ目の前後でノイズのように横ずれ・色ずれして切り替わる。strength=強さ、seed", dur=0.15, strength=1.0, seed=7)
def _t_glitch(E, tau, P):
    import numpy as np
    dur, W, H = float(P["dur"]), E.W, E.H
    src = E.src_frame("A" if tau < 0 else "B", tau)
    g = float(P["strength"]) * max(0.0, 1 - abs(tau) / dur)
    rnd = random.Random(int(P["seed"]) * 1000 + int(round(tau * E.fps)) + 500)
    out = src.copy()
    y = 0
    while y < H:  # 横の帯ごとにずらす
        hh = rnd.randint(int(H * 0.01) + 1, int(H * 0.12) + 2)
        if rnd.random() < 0.55 * g + 0.05:
            out[y:y + hh] = np.roll(src[y:y + hh], int(rnd.uniform(-1, 1) * g * W * 0.08), axis=1)
        y += hh
    d = int(round(g * 18 * E.k))  # 色ずれ
    if d:
        out[..., 0] = np.roll(out[..., 0], d, axis=1)
        out[..., 2] = np.roll(out[..., 2], -d, axis=1)
    return E.opaque(out)


for _n in ("crossfade", "push-left", "push-right", "push-up", "push-down", "zoom", "whip", "glitch"):
    REG["trans"][_n]["src"] = True


def _clip(part, clip, feather):
    """中身の外枠 part(RGBA)のうち clip (左,上,右,下)(比)の外を透明に。feather(px)で境目をぼかす"""
    import numpy as np
    h, w = part.shape[:2]
    fe = max(1e-3, feather)
    xs = np.arange(w, dtype=np.float32) + 0.5
    ys = np.arange(h, dtype=np.float32) + 0.5
    one_x, one_y = np.ones(w, np.float32), np.ones(h, np.float32)
    mx = (np.clip((xs - clip[0] * w) / fe, 0, 1) if clip[0] > -9 else one_x) * (np.clip((clip[2] * w - xs) / fe, 0, 1) if clip[2] < 10 else one_x)
    my = (np.clip((ys - clip[1] * h) / fe, 0, 1) if clip[1] > -9 else one_y) * (np.clip((clip[3] * h - ys) / fe, 0, 1) if clip[3] < 10 else one_y)
    out = part.copy()
    out[..., 3] = (part[..., 3].astype(np.float32) * my[:, None] * mx[None, :]).astype(np.uint8)
    return out


# ---------------------------------------------------------------- 文字アニメーション(テロップの animator。1文字ずつ時間をずらして動かす)
# fn(st, u, tau, i, n, P, E): st = その文字の動き(St)、u = その文字の出の進み(0→1、out:true なら消える側も)、i = 文字の番号、n = 文字数
def _ta_pop(st, u, tau, i, n, P, E):
    s = max(0.0, _back(min(1.0, u), float(P["overshoot"]))) if u < 1 else 1.0
    st.sx *= s
    st.sy *= s
    st.op *= min(1.0, u * 4)


def _ta_fade(st, u, tau, i, n, P, E):
    st.op *= u


def _ta_rise(st, u, tau, i, n, P, E):
    st.dy += (1 - ease("ease-out", u)) * float(P["dist"]) * E.k
    st.op *= u


def _ta_drop(st, u, tau, i, n, P, E):
    st.dy -= (1 - _bounce(min(1.0, u))) * float(P["dist"]) * E.k
    st.op *= min(1.0, u * 3)


def _ta_wave(st, u, tau, i, n, P, E):
    st.dy += math.sin(2 * math.pi * float(P["freq"]) * tau - i * float(P["phase"])) * float(P["amp"]) * E.k
    st.op *= u


def _ta_shake(st, u, tau, i, n, P, E):
    r = random.Random(int(P["seed"]) * 1000003 + i * 7919 + int(round(tau * E.fps)) // max(1, int(P["every"])))
    st.dx += r.uniform(-1, 1) * float(P["amp"]) * E.k
    st.dy += r.uniform(-1, 1) * float(P["amp"]) * E.k
    st.op *= u


_TA_COMMON = dict(stagger=0.05, dur=0.3, order="forward", out=False)
for _n, _fn, _d, _p in (
        ("pop", _ta_pop, "1文字ずつポンと出る(小さい→少し大きい→原寸)。overshoot=行き過ぎの強さ", dict(overshoot=2.2)),
        ("fade", _ta_fade, "1文字ずつ透明から出る", {}),
        ("rise", _ta_rise, "1文字ずつ下から浮かび上がる。dist=動く量(1080p 基準 px)", dict(dist=40)),
        ("drop", _ta_drop, "1文字ずつ上から落ちて弾む。dist=落ちる高さ(1080p 基準 px)", dict(dist=80)),
        ("wave", _ta_wave, "文字が波打つ(出た後もずっと)。amp=揺れ幅(px)、freq=1秒の波の数、phase=隣の文字との波のずれ", dict(amp=12, freq=1.2, phase=0.5)),
        ("shake", _ta_shake, "文字がばらばらに震える(出た後もずっと)。amp=揺れ幅(px)、every=何コマごとに動かすか、seed", dict(amp=4, every=2, seed=5))):
    reg("text", _n, _d + "。共通: stagger=次の文字までの秒、dur=1文字の出の秒、order=forward|backward|center|random、out=true で同じ順に消える",
        **_TA_COMMON, **_p)(_fn)


def _char_order(n, order, seed=11):
    idx = list(range(n))
    if order == "backward":
        idx = idx[::-1]
    elif order == "center":
        c = (n - 1) / 2
        idx = sorted(idx, key=lambda i: abs(i - c))
    elif order == "random":
        random.Random(seed).shuffle(idx)
    rank = [0] * n
    for r, i in enumerate(idx):
        rank[i] = r
    return rank


def _paste(dst, arr, x0, y0):
    """arr(RGBA)を dst(RGBA)の (x0, y0) に重ねる(はみ出しは切る)"""
    H, W = dst.shape[:2]
    h, w = arr.shape[:2]
    sx0, sy0 = max(0, -x0), max(0, -y0)
    dx0, dy0 = max(0, x0), max(0, y0)
    w2, h2 = min(w - sx0, W - dx0), min(h - sy0, H - dy0)
    if w2 <= 0 or h2 <= 0:
        return
    reg_ = dst[dy0:dy0 + h2, dx0:dx0 + w2]
    dst[dy0:dy0 + h2, dx0:dx0 + w2] = over(reg_, arr[sy0:sy0 + h2, sx0:sx0 + w2])


def _transform(part, st, center):
    """中身の外枠 part(RGBA)を st で変形し、置く左上 (x0, y0) と絵を返す。center = 動かす前の外枠の中心(画面の px)"""
    import numpy as np
    from PIL import Image
    if st.clip != St.CLIP0:
        part = _clip(part, st.clip, st.feather)
    crop = Image.fromarray(part)
    w0, h0 = crop.size
    w, h = max(1, round(w0 * st.sx)), max(1, round(h0 * st.sy))
    if (w, h) != crop.size:
        crop = crop.resize((w, h), Image.BICUBIC)
    if st.rot:
        crop = crop.rotate(-st.rot, Image.BICUBIC, expand=True)
    if st.blur >= 0.5:
        from PIL import ImageFilter
        p = int(math.ceil(st.blur * 3))
        big = Image.new("RGBA", (crop.width + 2 * p, crop.height + 2 * p))
        big.paste(crop, (p, p))
        crop = big.convert("RGBa").filter(ImageFilter.GaussianBlur(st.blur)).convert("RGBA")
    arr = np.asarray(crop)
    if st.op < 1:
        arr = arr.copy()
        arr[..., 3] = (arr[..., 3].astype(np.float32) * max(0.0, st.op)).astype(np.uint8)
    # 拡大・回転の中心(anchor)は動かない: 中心 = anchor + ずれ + 回した (中心 - anchor)
    vx, vy = (0.5 - st.ax) * w, (0.5 - st.ay) * h
    a = math.radians(st.rot)
    ca, sa = math.cos(a), math.sin(a)
    pax, pay = center[0] + (st.ax - 0.5) * w0, center[1] + (st.ay - 0.5) * h0
    cx, cy = pax + vx * ca - vy * sa + st.dx, pay + vx * sa + vy * ca + st.dy
    return round(cx - arr.shape[1] / 2), round(cy - arr.shape[0] / 2), arr


def _affine(st, pa):
    """st の変形(pa = 拡大・回転の中心の画面 px)を 2x3 の行列に"""
    import numpy as np
    a = math.radians(st.rot)
    A = np.array([[math.cos(a), -math.sin(a)], [math.sin(a), math.cos(a)]]) @ np.diag([st.sx, st.sy])
    p = np.array(pa, float)
    t = p + np.array([st.dx, st.dy]) - A @ p
    return np.hstack([A, t[:, None]])


def _bbox(c):
    import numpy as np
    ys, xs = np.nonzero(c[..., 3])
    if not len(xs):
        return None
    return (int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1)


class _SrcReader:
    """V1(元の動画をカットした並び)の出力時刻 t のコマ(RGB)。前へ順に読む。空白・黒にした所・V1 非表示は黒。
    戻る/2秒より先へ飛ぶ時は読み直す。V1 の揺れ(v1_fx の shake)も同じ式でかける"""

    def __init__(self, plan, W, H, shake=True):
        self.plan, self.W, self.H, self.fps = plan, W, H, float(plan.fps)
        self.p, self.s0, self.i, self.last = None, None, 0, None
        self.items = v1_items(plan) if shake else []
        self.zoom = v1_zoom(plan, self.items) if self.items else 1.0

    def src_time(self, t):
        acc = 0.0
        for pc in self.plan.pieces:
            L = pc[1] if pc[0] == "gap" else pc[2] - pc[1]
            if t < acc + L - 1e-9:
                return None if pc[0] == "gap" else pc[1] + (t - acc)
            acc += L
        return None

    def black(self):
        import numpy as np
        return np.zeros((self.H, self.W, 3), np.uint8)

    def raw(self, s):
        import numpy as np
        idx = int(round(s * self.fps))
        if self.p is None or idx < self.i or idx - self.i > 2 * self.fps:
            self.close()
            src = self.plan.proj.resolve(self.plan.proj.data["source"])
            self.p = subprocess.Popen([ff.find("ffmpeg"), "-v", "error", "-ss", f"{idx / self.fps:.4f}", "-i", str(src), "-vf",
                                       f"fps={self.fps},scale={self.W}:{self.H}", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
                                      stdout=subprocess.PIPE, creationflags=ff.NOWIN)
            self.i, self.last = idx, None
        n = self.W * self.H * 3
        while self.i <= idx:
            buf = self.p.stdout.read(n)
            if len(buf) == n:
                self.last = buf
            self.i += 1
        if self.last is None:
            return self.black()
        return np.frombuffer(self.last, np.uint8).reshape(self.H, self.W, 3)

    def frame(self, t):
        if "v1" in self.plan.hide or any(a <= t < b for a, b in self.plan.black):
            return self.black()
        s = self.src_time(t)
        if s is None:
            return self.black()
        f = self.raw(s)
        for o, e, P in self.items:  # 背景の揺れ(v1_graph と同じ: 少し拡大してずらす)
            if o <= t < e:
                import numpy as np
                import cv2
                k = self.H / 1080.0
                dx, dy = shake_xy(t - o, P, k)
                z = self.zoom
                M = np.float32([[z, 0, (1 - z) * self.W / 2 + dx], [0, z, (1 - z) * self.H / 2 + dy]])
                f = cv2.warpAffine(np.ascontiguousarray(f), M, (self.W, self.H), flags=cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
                break
        return f

    def close(self):
        if self.p:
            self.p.kill()
            self.p = None


class _Below:
    """調整レイヤー・合成モード用: 出力時刻 t の「その要素より下」の絵(V1 + 下の差し込み)"""

    def __init__(self, plan, items, v1groups, W, H):
        self.plan, self.items, self.v1groups, self.W, self.H = plan, items, v1groups, W, H
        self.src = None
        self.elems = {}

    def _elem(self, key, it, a, b):
        if key not in self.elems:
            self.elems[key] = Elem(self.plan, it, a, b)
        return self.elems[key]

    def frame(self, t):
        import numpy as np
        out = None
        for j, (a, b, it) in enumerate(self.v1groups):
            if a <= t < b and not it.get("_alpha"):  # 画面全体を変える背景のエフェクト
                out = self._elem(("v", j), it, a, b).render(t - a)
                break
        if out is None:
            if self.src is None:
                self.src = _SrcReader(self.plan, self.W, self.H)
            out = np.empty((self.H, self.W, 4), np.uint8)
            out[..., :3] = self.src.frame(t)
            out[..., 3] = 255
            for j, (a, b, it) in enumerate(self.v1groups):
                if a <= t < b and it.get("_alpha"):  # 範囲だけ変える背景のエフェクト
                    out = over(out, self._elem(("v", j), it, a, b).render(t - a))
        for j, (a, b, it) in enumerate(self.items):
            if a <= t < b:
                out = over(out, self._elem(("o", j), it, a, b).render(t - a))
        return out

    def close(self):
        if self.src:
            self.src.close()
        for E in self.elems.values():
            E.close()


# ---------------------------------------------------------------- 要素(1つの差し込みを描く)
class Elem:
    """overlays の1要素(画像/動画/生成/調整レイヤー/プリコンポーズ/背景のエフェクト)やテロップを、出力上の a〜b 秒に描く。"""

    def __init__(self, plan, it, a, b, trans=None):
        self.plan, self.proj, self.it, self.a, self.b = plan, plan.proj, it, a, b
        self.W, self.H = plan.W, plan.H
        if getattr(plan, "draft", False):  # 低画質の書き出し: 小さく描いて ffmpeg で画面の大きさに戻す
            self.W, self.H = 640, max(2, round(640 * plan.H / plan.W / 2) * 2)
        self.fps = float(plan.fps)
        self.k = self.H / 1080.0
        self.trans = trans  # (名前, P, 継ぎ目の出力時刻, (A, B) 継ぎ目の元素材の時刻) トランジションの時
        self._memo = {}
        self._tau = 0.0
        self.telop = bool(it.get("_telop")) and not trans
        kind = it.get("kind")
        self.special = kind if kind in ("adjust", "comp", "_v1") and not trans and not it.get("file") else None
        if self.special == "comp" and it.get("project"):
            self.special = None
            it = {**it, "file": str(_comp_project(plan, it)), "kind": None}
            self.it = it
        self.gen = None if (trans or it.get("file") or self.telop or self.special) else kind
        self.file = self.proj.resolve(it["file"]) if it.get("file") and not trans else None
        self.is_video = bool(self.file) and self.file.suffix.lower() not in (".png", ".jpg", ".jpeg", ".webp", ".bmp")
        self.anims = []
        self.typer = None  # テロップの typewriter: [(side, interval)]
        for side in ("in", "out"):
            spec = it.get(side + "_anim")
            if not spec or spec == "none":
                continue
            name = spec["type"] if isinstance(spec, dict) else spec
            P = params("anim", name, spec if isinstance(spec, dict) else {})
            if "anim_dur" in it and not (isinstance(spec, dict) and "dur" in spec):
                P["dur"] = it["anim_dur"]
            if name == "typewriter":
                self.typer = (self.typer or []) + [(side, max(1e-3, float(P["interval"])))]
                continue
            dur = min(float(P["dur"]), (b - a) / (2 if it.get("in_anim") not in (None, "none") and it.get("out_anim") not in (None, "none") else 1))
            self.anims.append((side, name, P, max(1e-3, dur)))
        self.keys = {k: v for k, v in (it.get("keys") or {}).items()}
        self.fxs = []
        for i, f in enumerate(it.get("fx") or []):
            P = params("fx", f["type"], f)
            for kk, v in self.keys.items():  # keys の "mosaic.rect" / "fx[0].rect" は fx のパラメータを動かす
                if "." in kk:
                    tgt, prm = kk.split(".", 1)
                    if tgt in (f["type"], f"fx[{i}]"):
                        P[prm] = v
            s = parse_time(f.get("start", 0))
            e = parse_time(f["end"]) if f.get("end") is not None else None
            self.fxs.append((f["type"], P, s, e, has_dyn(P), _seed_of(f["type"], i)))
        self.gP = params("gen", self.gen, it.get("params")) if self.gen else None
        if self.gen and has_dyn(self.gP):
            self._gdyn = True
        else:
            self._gdyn = False
        self.mask = it.get("mask") or None
        if isinstance(self.mask, dict):
            self.mask = [self.mask]
        self.blend = it.get("blend") or "normal"
        an = it.get("animator") if self.telop else None
        self.animator = ([an] if isinstance(an, dict) else list(an)) if an else None
        # expand が後から付けるもの
        self.matte = None    # (Elem, mode)
        self.parents = []    # [Elem](近い親から順)
        self.below = None    # _Below(調整レイヤー・合成モード)
        self.alpha_diff = self.special == "_v1" and bool(it.get("_alpha"))  # 背景のエフェクト: 変わった所だけ不透明に
        self.solid = self.special == "_v1" and not self.alpha_diff  # 不透明の動画(x264)で焼く
        self.deps = []       # キャッシュの鍵に足すもの
        self.layers = None
        if self.special == "comp":
            self.layers = [(parse_time(L.get("start", 0)), parse_time(L["end"]) if L.get("end") is not None else (b - a),
                            {**L, "_telop": True} if L.get("text") is not None and not L.get("file") and not L.get("kind") else L)
                           for L in it.get("layers") or []]
            self.layers.sort(key=lambda x: max(2, round(float(x[2].get("track") or 2))) if not x[2].get("_telop") else TEL_TRACK)

    # --- 補助 ---
    def memo(self, key, fn):
        if key not in self._memo:
            self._memo[key] = fn()
        return self._memo[key]

    def rand(self, name, seed, n):
        return self.memo(("rand", name, seed, n), lambda: [(lambda r: [r.uniform(-1, 1) for _ in range(n)])(random.Random(seed))][0])

    @property
    def rect(self):
        """絵を置く場所 (x, y, w, h)(render.py の _scale_filter / _overlay_pos と同じ)"""
        return self.memo("rect", self._rect)

    def _rect(self):
        if not self.file:
            return (0, 0, self.W, self.H)
        iw, ih = self.src_size
        it, W, H = self.it, self.W, self.H
        pos = it.get("pos", "full" if it.get("scale") is None else "top-right")
        if pos == "full":
            k = (max if it.get("fit") == "cover" else min)(W / iw, H / ih)
            w, h = round(iw * k), round(ih * k)
            return ((W - w) // 2, (H - h) // 2, w, h)
        w = int(W * float(it.get("scale", 0.3)) / 2) * 2
        h = max(2, round(ih * w / iw / 2) * 2)
        m = round(40 * self.k)
        if isinstance(pos, (list, tuple)):
            return (round(pos[0] * self.k), round(pos[1] * self.k), w, h)
        x = {"center": (W - w) / 2, "top-left": m, "bottom-left": m}.get(pos, W - w - m)
        y = {"center": (H - h) / 2, "top-left": m, "top-right": m}.get(pos, H - h - m)
        return (round(x), round(y), w, h)

    @property
    def src_size(self):
        def get():
            if self.is_video:
                i = ff.probe(self.file)
                return i["width"], i["height"]
            from PIL import Image
            with Image.open(self.file) as im:
                return im.size
        return self.memo("size", get)

    # --- 中身の絵 ---
    def animated_content(self):
        if self.trans or self.is_video or self.special or self.animator or self._gdyn:
            return True
        if self.gen:
            a = REG["gen"][self.gen].get("animated")
            return bool(a and a(self.gP))
        return False

    def content(self, tau):
        """τ 秒の中身(画面全体の RGBA)。"""
        import numpy as np
        if self.trans:
            name, P = self.trans[:2]
            return REG["trans"][name]["fn"](self, tau, P)
        if self.special == "_v1":
            return self.opaque(self.memo("v1src", lambda: _SrcReader(self.plan, self.W, self.H)).frame(self.a + tau))
        if self.special == "adjust":
            return self.below.frame(self.a + tau) if self.below else np.zeros((self.H, self.W, 4), np.uint8)
        if self.special == "comp":
            return self._comp_frame(tau)
        if self.telop:
            if self.animator:
                return self._animated_telop(tau)
            imgs = self.memo("telop", self._telop_imgs)
            return imgs[self.type_count(tau)] if self.typer else imgs[-1]
        if self.gen:
            fn = REG["gen"][self.gen]["fn"]
            if self._gdyn:
                return fn(self, tau, pval(self.gP, tau, self, _seed_of(self.gen)))
            if self.animated_content():
                return fn(self, tau, self.gP)
            return self.memo("gen", lambda: fn(self, 0.0, self.gP))
        if self.is_video:
            return self._video_frame(tau)
        return self.memo("img", self._image_canvas)

    def _comp_frame(self, tau):
        """プリコンポーズ: layers(時刻はこの要素の頭から)を重ねた絵"""
        import numpy as np
        subs = self.memo("comp", lambda: [Elem(self.plan, it, a, b) for a, b, it in self.layers])
        out = np.zeros((self.H, self.W, 4), np.uint8)
        for E in subs:
            if E.a <= tau < E.b:
                out = over(out, E.render(tau - E.a))
        return out

    def _place(self, im):
        """PIL 画像を置き場所に縮めて画面全体の RGBA に。"""
        import numpy as np
        from PIL import Image
        x, y, w, h = self.rect
        it = self.it
        if (w, h) != im.size:
            im = im.resize((w, h), Image.BICUBIC)
        cv = Image.new("RGBA", (self.W, self.H))
        pos = it.get("pos", "full" if it.get("scale") is None else "top-right")
        if pos == "full" and it.get("fit") != "cover" and self.is_video and (w, h) != (self.W, self.H):
            cv = Image.new("RGBA", (self.W, self.H), (0, 0, 0, 255))  # render.py の pad(黒)と同じ
        cv.paste(im, (x, y))  # 空の画面に置くだけなので重ねない(透明もそのまま写す)
        return np.asarray(cv).copy()

    def _image_canvas(self):
        from PIL import Image
        with Image.open(self.file) as im:
            return self._place(im.convert("RGBA"))

    # --- テロップ(動きを付けたものだけここで描く。絵は書き出しと同じ libass で作る) ---
    def _tw_cuts(self):
        """typewriter で1文字ずつ見せる区切り(空白・改行は数えない)"""
        return [i for i, ch in enumerate(str(self.it.get("text", ""))) if not ch.isspace()]

    def _ass_render(self, variants):
        """テロップの変種を透明の上に libass で描く(1回の ffmpeg で全部) → [画面全体の RGBA]"""
        import numpy as np
        from . import ass
        txt = ass.build(self.proj, self.W, self.H, [(j * 0.1, (j + 1) * 0.1, v) for j, v in enumerate(variants)], [])
        cdir = self.proj.cache_dir / "fx"
        cdir.mkdir(parents=True, exist_ok=True)
        name = "tel_" + hashlib.sha1(txt.encode("utf-8")).hexdigest()[:16] + ".ass"
        (cdir / name).write_text(txt, encoding="utf-8")
        n, W, H = len(variants), self.W, self.H
        r = subprocess.run([ff.find("ffmpeg"), "-v", "error", "-f", "lavfi", "-i", f"color=c=black@0.0:s={W}x{H}:r=10:d={n * 0.1 + 0.1:.1f},format=rgba",
                            "-vf", f"ass={name}:alpha=1", "-frames:v", str(n), "-f", "rawvideo", "-pix_fmt", "rgba", "-"],
                           cwd=cdir, capture_output=True, creationflags=ff.NOWIN)
        sz = W * H * 4
        if r.returncode or len(r.stdout) < sz * n:
            raise RuntimeError(f"テロップの描画に失敗しました: {r.stderr.decode('utf-8', 'replace')[-400:]}")
        return [np.frombuffer(r.stdout, np.uint8, sz, i * sz).reshape(H, W, 4) for i in range(n)]

    def _telop_base(self):
        base = {k: v for k, v in self.it.items() if k in ("text", "style", "pos")}
        base["anim"] = "none"
        return base

    def _telop_imgs(self):
        """テロップの絵(画面全体の RGBA)。typewriter なら [0文字, 1文字, …, 全部]、それ以外は [全部]"""
        base = self._telop_base()
        variants = [{**base, "_hide_from": c} for c in (self._tw_cuts() if self.typer else [])] + [base]
        return self._ass_render(variants)

    def _char_imgs(self):
        """文字アニメーション用: 1文字ずつの [(縁の (x0,y0,絵), 文字の (x0,y0,絵), 外枠の中心)](空白は数えない)"""
        base = self._telop_base()
        cuts = self._tw_cuts()
        variants = []
        for c in cuts:
            variants += [{**base, "_show_only": c, "_layers": "outline"}, {**base, "_show_only": c, "_layers": "fill"}]
        imgs = self._ass_render(variants) if variants else []
        out = []
        for j in range(len(cuts)):
            o, f = imgs[2 * j], imgs[2 * j + 1]
            bb = _bbox(o) or _bbox(f)
            if not bb:
                out.append(None)
                continue
            x0, y0, x1, y1 = bb
            out.append(((x0, y0, o[y0:y1, x0:x1].copy()), (x0, y0, f[y0:y1, x0:x1].copy()), ((x0 + x1) / 2, (y0 + y1) / 2)))
        return out

    def _animated_telop(self, tau):
        import numpy as np
        chars = self.memo("chars", self._char_imgs)
        n, L = len(chars), self.b - self.a
        sts = []
        for i in range(n):
            st = St()
            for A in self.animator:
                P = params("text", A["type"], A)
                rank = _char_order(n, P["order"])
                dur = max(1e-3, float(P["dur"]))
                d0 = float(P["stagger"]) * rank[i]
                u = min(1.0, max(0.0, (tau - d0) / dur))
                if P["out"]:
                    u = min(u, max(0.0, (L - tau - d0) / dur))
                REG["text"][A["type"]]["fn"](st, u, tau, i, n, P, self)
            sts.append(st)
        out = np.zeros((self.H, self.W, 4), np.uint8)
        for layer in (0, 1):  # 縁を全部描いてから文字を全部(普通のテロップと同じ重なり)
            for ch, st in zip(chars, sts):
                if ch is None or st.op <= 0.002 or st.sx <= 0.002 or st.sy <= 0.002:
                    continue
                x0, y0, img = ch[layer]
                cx, cy = ch[2]
                # 縁と文字は同じ点(縁の外枠の中心)を中心に動かす。_transform は絵の中心を中心にするので差を足す
                ox, oy = x0 + img.shape[1] / 2, y0 + img.shape[0] / 2
                px, py, arr = _transform(img, st, (ox, oy))
                vx, vy = (ox - cx) * st.sx, (oy - cy) * st.sy
                ra = math.radians(st.rot)
                px += round(vx * math.cos(ra) - vy * math.sin(ra) - (ox - cx))
                py += round(vx * math.sin(ra) + vy * math.cos(ra) - (oy - cy))
                _paste(out, arr, px, py)
        return out

    def type_count(self, tau):
        """typewriter: τ 秒に見せる文字数"""
        n, L = len(self._tw_cuts()), self.b - self.a
        k = n
        for side, iv in self.typer:
            q = tau if side == "in" else L - tau
            k = min(k, int(q / iv + 1e-6) + (1 if side == "in" else 0))
        return max(0, min(n, k))

    # --- トランジションの前後の映像 ---
    def src_frame(self, side, tau):
        """継ぎ目から τ 秒の、前(A)/後(B)の元素材のコマ(RGB)。継ぎ目の外はカットされた部分の続き。空白・V1 非表示は黒"""
        import numpy as np
        j = self.trans[3][0 if side == "A" else 1]
        if j is None or "v1" in self.plan.hide:
            return self.memo("black", lambda: np.zeros((self.H, self.W, 3), np.uint8))
        key = "src" + side
        idx = max(0, int(round((j + tau) * self.fps)))
        r = self._memo.get(key)
        if r is None or idx < r["i"]:
            if r:
                r["p"].kill()
            src = self.proj.resolve(self.proj.data["source"])
            p = subprocess.Popen([ff.find("ffmpeg"), "-v", "error", "-ss", f"{idx / self.fps:.4f}", "-i", str(src), "-vf",
                                  f"fps={self.fps},scale={self.W}:{self.H}", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
                                 stdout=subprocess.PIPE, creationflags=ff.NOWIN)
            r = self._memo[key] = {"p": p, "i": idx, "last": None}
        n = self.W * self.H * 3
        while r["i"] <= idx:
            buf = r["p"].stdout.read(n)
            if len(buf) == n:
                r["last"] = buf
            r["i"] += 1
        if r["last"] is None:
            return self.memo("black", lambda: np.zeros((self.H, self.W, 3), np.uint8))
        return np.frombuffer(r["last"], np.uint8).reshape(self.H, self.W, 3)

    def opaque(self, rgb):
        import numpy as np
        out = np.empty((self.H, self.W, 4), np.uint8)
        out[..., :3] = rgb
        out[..., 3] = 255
        return out

    def _video_frame(self, tau):
        """差し込み動画の τ 秒のコマ(頭から順に読む)。"""
        import numpy as np
        from PIL import Image
        r = self._memo.get("vr")
        idx = int(round(tau * self.fps))
        if r is None or idx < r["i"]:
            if r:
                r["p"].kill()
            iw, ih = self.src_size
            st = parse_time(self.it.get("in", 0)) + max(0.0, tau)
            p = subprocess.Popen([ff.find("ffmpeg"), "-v", "error", "-ss", f"{st:.4f}", "-i", str(self.file), "-vf", f"fps={self.fps}",
                                  "-f", "rawvideo", "-pix_fmt", "rgba", "-"], stdout=subprocess.PIPE, creationflags=ff.NOWIN)
            r = self._memo["vr"] = {"p": p, "i": idx, "last": None, "size": (iw, ih)}
        iw, ih = r["size"]
        while r["i"] <= idx:
            buf = r["p"].stdout.read(iw * ih * 4)
            if len(buf) == iw * ih * 4:
                r["last"] = buf
            r["i"] += 1
        if r["last"] is None:
            return np.zeros((self.H, self.W, 4), np.uint8)
        return self._place(Image.frombuffer("RGBA", (iw, ih), r["last"], "raw", "RGBA", 0, 1))

    def close(self):
        for k in ("vr", "srcA", "srcB"):
            r = self._memo.pop(k, None)
            if r:
                r["p"].kill()
        for k in ("v1src",):
            r = self._memo.pop(k, None)
            if r:
                r.close()
        for E in self._memo.pop("comp", None) or []:
            E.close()
        if self.below:
            self.below.close()
        for E in [m for m in (self.matte or ())[:1]] + list(self.parents):
            E.close()

    # --- 動き ---
    def state(self, tau):
        """τ 秒の動き(St)と、絵を変えるエフェクトの一覧 [(名前, P(値は τ 秒のもの), fx の頭からの秒)]。"""
        st, L = St(), self.b - self.a
        for side, name, P, dur in self.anims:
            q = tau if side == "in" else L - tau
            if q < dur:
                REG["anim"][name]["fn"](st, max(0.0, q), dur, P, self)
        if self.keys:
            k = self.k
            for prop in KEY_PROPS:
                if prop not in self.keys:
                    continue
                v = float(pval(self.keys[prop], tau, self, _seed_of(prop)))
                if prop == "x":
                    st.dx += v * k
                elif prop == "y":
                    st.dy += v * k
                elif prop == "scale":
                    st.sx *= v
                    st.sy *= v
                elif prop == "scale_x":
                    st.sx *= v
                elif prop == "scale_y":
                    st.sy *= v
                elif prop == "rot":
                    st.rot += v
                elif prop == "opacity":
                    st.op *= max(0.0, min(1.0, v))
                elif prop == "anchor_x":
                    st.ax = v
                elif prop == "anchor_y":
                    st.ay = v
        pix = []
        for name, P, s, e, dyn, seed in self.fxs:
            if tau < s or (e is not None and tau >= e):
                continue
            if dyn:
                P = pval(P, tau - s, self, seed)
            P = {**P, "_len": (e if e is not None else L) - s}
            spec = REG["fx"][name]
            if spec.get("kind") == "state":
                spec["fn"](st, tau - s, P, self)
            else:
                pix.append((name, P, tau - s))
        return st, pix

    def mask_at(self, tau):
        """マスク(画面全体の 0〜1)。無ければ None"""
        if not self.mask:
            return None
        m = None
        for i, spec in enumerate(self.mask):
            s = pval(spec, tau, self, _seed_of("mask", i)) if has_dyn(spec) else spec
            f = shape_mask(self, s)
            mode = s.get("mode", "add")
            if m is None:
                m = 1 - f if mode == "subtract" else f
            elif mode == "subtract":
                m = m * (1 - f)
            elif mode == "intersect":
                m = m * f
            else:
                import numpy as np
                m = np.maximum(m, f)
        if self.it.get("mask_opacity") is not None:
            m = m * float(self.it["mask_opacity"])
        return m

    def sig(self, tau):
        """このコマの見た目を決めるもの。前後のコマと同じなら絵も同じ(静止部分を1枚の PNG にする)。None=毎コマ違う"""
        if self.animated_content() or self.matte or self.parents or self.below or self.blend != "normal":
            return None
        st, pix = self.state(tau)
        ms = json.dumps(pval(self.mask, tau, self) if has_dyn(self.mask) else None, sort_keys=True, default=str) if self.mask else ""
        return (st.sig(), self.type_count(tau) if self.typer else 0, ms,
                tuple((n, json.dumps(P, sort_keys=True, default=str), REG["fx"][n].get("sig", lambda t, P: 0)(t, P)) for n, P, t in pix))

    def anchor(self):
        """拡大の中心: 中身(不透明な所)の中心"""
        def get():
            c = self.memo("telop", self._telop_imgs)[-1] if (self.telop and not self.animator) else self.content(0.0)
            bb = _bbox(c)
            if not bb:
                return (self.W / 2, self.H / 2, (0, 0, self.W, self.H))
            return ((bb[0] + bb[2]) / 2, (bb[1] + bb[3]) / 2, bb)
        return self.memo("anchor", get)

    def _bb_of(self, c):
        if self.animated_content():
            bb = _bbox(c) or (0, 0, 1, 1)
            return (bb[0] + bb[2]) / 2, (bb[1] + bb[3]) / 2, bb
        return self.anchor()

    def render(self, tau, no_mb=False):
        """τ 秒の1コマ(画面全体の RGBA uint8)"""
        import numpy as np
        c = self.content(tau)
        base = c
        m = self.mask_at(tau)
        if m is not None and self.special != "adjust":  # マスクは動かす前の中身にかける(調整レイヤーはエフェクトの後)
            c = c.copy()
            c[..., 3] = (c[..., 3].astype(np.float32) * m + 0.5).astype(np.uint8)
        st, pix = self.state(tau)
        if no_mb:
            pix = [p for p in pix if p[0] != "motion-blur"]
        if not st.identity() and self.special != "adjust":
            cx, cy, bb = self._bb_of(c)
            part = c[bb[1]:bb[3], bb[0]:bb[2]]
            x0, y0, arr = _transform(part, st, (cx, cy))
            out = np.zeros_like(c)
            sx0, sy0 = max(0, -x0), max(0, -y0)
            dx0, dy0 = max(0, x0), max(0, y0)
            w2 = min(arr.shape[1] - sx0, self.W - dx0)
            h2 = min(arr.shape[0] - sy0, self.H - dy0)
            if w2 > 0 and h2 > 0:
                out[dy0:dy0 + h2, dx0:dx0 + w2] = arr[sy0:sy0 + h2, sx0:sx0 + w2]
            c = out
        self._tau = tau
        for name, P, t in pix:
            r = REG["fx"][name]["fn"](c, t, P, self)
            rg = _region_of(P)
            c = _mixm(c, r, shape_mask(self, rg)) if rg else r
        if self.special == "adjust":
            a = (m if m is not None else 1.0) * max(0.0, min(1.0, st.op))
            if not isinstance(a, float) or a < 1:
                c = c.copy()
                c[..., 3] = (c[..., 3].astype(np.float32) * a + 0.5).astype(np.uint8)
        if self.matte:
            ME, mode = self.matte
            T = self.a + tau
            if ME is not None and ME.a <= T < ME.b:
                mc = ME.render(T - ME.a)
                mv = mc[..., 3].astype(np.float32) / 255
                if mode.startswith("luma"):
                    mv = mv * _luma(mc[..., :3].astype(np.float32) / 255)
            else:
                mv = np.zeros(c.shape[:2], np.float32)
            if mode.endswith("-inv"):
                mv = 1 - mv
            c = c.copy()
            c[..., 3] = (c[..., 3].astype(np.float32) * mv + 0.5).astype(np.uint8)
        if self.parents:  # 親の動き(拡大・回転・移動。透明度は受け継がない)を近い親から順にかける
            import cv2
            M = np.array([[1.0, 0, 0], [0, 1.0, 0], [0, 0, 1.0]])
            T = self.a + tau
            for PE in self.parents:
                pt = min(max(0.0, T - PE.a), max(0.0, PE.b - PE.a - 1e-6))
                pst, _ = PE.state(pt)
                cx, cy, bb = PE.anchor()
                pa = (bb[0] + pst.ax * (bb[2] - bb[0]), bb[1] + pst.ay * (bb[3] - bb[1]))
                M = np.vstack([_affine(pst, pa), [0, 0, 1]]) @ M
            if not np.allclose(M, np.eye(3)):
                c = _unpm(cv2.warpAffine(_pm(c), M[:2].astype(np.float32), (self.W, self.H), flags=cv2.INTER_LINEAR))
        if self.blend != "normal" and self.below is not None:
            b = self.below.frame(self.a + tau)[..., :3].astype(np.float32) / 255
            s = c[..., :3].astype(np.float32) / 255
            c = c.copy()
            c[..., :3] = np.clip(BLEND[self.blend](b, s) * 255 + 0.5, 0, 255).astype(np.uint8)
        if self.alpha_diff:  # 背景の範囲だけのエフェクト: 変わらなかった所は透明(下の V1 がそのまま見える)
            d = np.any(c[..., :3] != base[..., :3], axis=2)
            c = c.copy()
            c[..., 3] = np.where(d, 255, 0).astype(np.uint8)
        return c

    # --- キャッシュの鍵 ---
    def key(self):
        it = {k: v for k, v in self.it.items() if k not in ("start", "end", "gap_at", "track", "text", "name", "_oid", "id", "parent")}
        if self.telop:  # テロップは文字・スタイル(プロジェクトの styles 込み)で絵が変わる
            it.update(text=self.it.get("text"), _style=self.proj.style(self.it.get("style", "telop")))
        st = None
        if self.file:
            s = self.file.stat()
            st = [str(self.file), s.st_mtime_ns, s.st_size]
        dep = []
        if self.special == "_v1" or self.special == "adjust" or self.below is not None:
            src = self.proj.resolve(self.proj.data["source"])
            s = src.stat()
            dep.append([str(src), s.st_mtime_ns, s.st_size, _src_map(self.plan, self.a, self.b), sorted(self.plan.hide),
                        [x for x in self.plan.black if x[0] < self.b and x[1] > self.a],
                        [(o, e, P) for o, e, P in v1_items(self.plan) if o < self.b and e > self.a]])
        if self.special == "comp":
            for a, b, L in self.layers:
                if L.get("file"):
                    f = self.proj.resolve(L["file"])
                    if f.exists():
                        s = f.stat()
                        dep.append([str(f), s.st_mtime_ns, s.st_size])
                if L.get("_telop"):
                    dep.append(self.proj.style(L.get("style", "telop")))
        dep += self.deps
        return hashlib.sha1(json.dumps([VERSION, it, st, self.W, self.H, self.fps, self.trans, round(self.b - self.a, 4), dep, self.alpha_diff],
                                       sort_keys=True, default=str).encode()).hexdigest()[:16]


def _src_map(plan, a, b):
    """出力 a〜b 秒に出る元素材の区間 [(出力の頭, 元素材の頭, 長さ)](V1 を使う要素のキャッシュの鍵)"""
    out, acc = [], 0.0
    for pc in plan.pieces:
        L = pc[1] if pc[0] == "gap" else pc[2] - pc[1]
        if acc < b and acc + L > a:
            out.append((round(acc, 4), round(pc[1], 4) if pc[0] == "src" else None, round(L, 4)))
        acc += L
    return out


_COMP_BUSY = set()


def _comp_project(plan, it):
    """kind:"comp" + project:"x.vedit.json": 別のプロジェクトを書き出した動画(キャッシュ)のパス"""
    from .project import Project
    from . import render
    p = plan.proj.resolve(it["project"])
    if str(p) in _COMP_BUSY or p.resolve() == Path(plan.proj.path).resolve():
        raise ValueError(f"プリコンポーズが自分自身を読み込んでいます: {p}")
    sub = Project(p)
    s = p.stat()
    h = hashlib.sha1(json.dumps([str(p), s.st_mtime_ns, s.st_size, plan.W, plan.H, plan.fps]).encode()).hexdigest()[:16]
    out = plan.proj.cache_dir / "fx" / f"comp_{h}.mp4"
    if not out.exists():
        _COMP_BUSY.add(str(p))
        try:
            out.parent.mkdir(parents=True, exist_ok=True)
            pl = render.Plan(sub)
            tmp = _tmp(out)
            render.render(sub, str(tmp), rng=(0.0, pl.total))
            tmp.replace(out)
        finally:
            _COMP_BUSY.discard(str(p))
    return out


# ---------------------------------------------------------------- 焼き付け(キャッシュ)
def needs_bake(it):
    """ffmpeg のフィルタだけでは描けず、ここで描く要素か(新しいキーが無い要素は今までどおり ffmpeg)"""
    if not it.get("file"):
        return it.get("kind") in REG["gen"] or it.get("kind") in ("adjust", "comp")
    if it.get("fx") or any(it.get(k) for k in NEW_KEYS + ("_preset",)) or it.get("blend") not in (None, "normal"):
        return True
    for side in ("in_anim", "out_anim"):
        v = it.get(side)
        name = v.get("type") if isinstance(v, dict) else v
        if isinstance(v, dict) or name not in (None, "none", "fade", "slide-left", "slide-right", "slide-up", "slide-down"):
            return True
    return False


LEGACY_TELOP_ANIM = (None, "none", "fade", "pop", "slide-up")  # ass.py が描く今までのテロップの anim
NEW_KEYS = ("keys", "mask", "matte", "parent")  # 付けるとここで焼く要素のキー


def needs_bake_telop(t):
    """テロップを ASS ではなくここで焼くか(fx・in_anim/out_anim・新しい動きの anim・keys 等を付けた時だけ。今までのテロップは ASS のまま)"""
    if t.get("fx") or isinstance(t.get("anim"), dict) or t.get("anim") not in LEGACY_TELOP_ANIM:
        return True
    if any(t.get(k) for k in NEW_KEYS + ("animator", "_preset")) or t.get("blend") not in (None, "normal"):
        return True
    return any(t.get(k) not in (None, "none") for k in ("in_anim", "out_anim"))


def telop_norm(t):
    """焼くテロップを overlays の要素と同じ形に(in_anim/out_anim にそろえる)"""
    it = {**t, "_telop": True, "track": TEL_TRACK, "_oid": id(t)}
    if t.get("in_anim") in (None, "none") and t.get("out_anim") in (None, "none"):
        a = t.get("anim")
        if isinstance(a, dict) or a not in LEGACY_TELOP_ANIM:
            it["in_anim"] = a
        elif a not in (None, "none"):  # fx と一緒に付いた今までの fade/pop/slide-up は出入りのフェードに
            it["in_anim"] = it["out_anim"] = "fade"
            it.setdefault("anim_dur", 0.2)
    return it


def telop_items(plan):
    """焼くテロップ(出力上の時刻) → [(a, b, it)]"""
    if "t" in plan.hide:
        return []
    hidden = matte_map(plan.proj.data)[1]
    ts = [telop_norm(t) for t in plan.proj.data.get("telops") or [] if str(t.get("text", "")).strip() and needs_bake_telop(t) and id(t) not in hidden]
    return plan.conv(ts)


def _write_mov(path, frames, W, H, fps):
    tmp = _tmp(path)
    p = subprocess.Popen([ff.find("ffmpeg"), "-y", "-v", "error", "-f", "rawvideo", "-pix_fmt", "rgba", "-s", f"{W}x{H}", "-r", str(fps),
                          "-i", "-", "-c:v", "qtrle", "-pix_fmt", "argb", str(tmp)], stdin=subprocess.PIPE, creationflags=ff.NOWIN)
    try:
        for f in frames:
            p.stdin.write(f.tobytes())
    finally:
        p.stdin.close()
        if p.wait():
            raise RuntimeError(f"ffmpeg(エフェクトの書き出し)が失敗しました: {path.name}")
    tmp.replace(path)


def _write_mp4(path, frames, W, H, fps):
    """不透明の絵(背景のエフェクト・調整レイヤー)は透明を持たない動画に(qtrle だと画面全体が毎コマ変わって巨大になる)"""
    tmp = _tmp(path)
    p = subprocess.Popen([ff.find("ffmpeg"), "-y", "-v", "error", "-f", "rawvideo", "-pix_fmt", "rgba", "-s", f"{W}x{H}", "-r", str(fps),
                          "-i", "-", "-c:v", "libx264", "-preset", "fast", "-crf", "10", "-pix_fmt", "yuv420p", "-g", str(max(1, round(fps / 2))),
                          str(tmp)], stdin=subprocess.PIPE, creationflags=ff.NOWIN)
    try:
        for f in frames:
            p.stdin.write(f.tobytes())
    finally:
        p.stdin.close()
        if p.wait():
            raise RuntimeError(f"ffmpeg(エフェクトの書き出し)が失敗しました: {path.name}")
    tmp.replace(path)


def _tmp(path):
    """書きかけの一時ファイル名(呼び出しごとに別。GUI のプレビューと書き出しが同時に同じものを作っても rename がぶつからない)。
    拡張子は残す(ffmpeg が形式を拡張子で決める)"""
    import threading
    return path.with_name(f"{path.stem}.tmp{os.getpid()}_{threading.get_ident()}{path.suffix}")


def plan_segments(E):
    """要素を出力のコマ単位に区切る → (a0, n, [(i0, i1, "anim"|"still", sig)])。a0 = 最初のコマの出力時刻"""
    fps = E.fps
    k0 = math.ceil(E.a * fps - 1e-6)
    k1 = math.ceil(E.b * fps - 1e-6)
    n = max(1, k1 - k0)
    a0 = k0 / fps
    taus = [a0 + i / fps - E.a for i in range(n)]
    sigs = [E.sig(t) for t in taus]
    runs, i = [], 0
    while i < n:
        j = i + 1
        while j < n and sigs[i] is not None and sigs[j] == sigs[i]:
            j += 1
        runs.append([i, j, "still" if sigs[i] is not None and (j - i) >= STATIC_MIN * fps else "anim", sigs[i]])
        i = j
    segs = []
    for r in runs:  # 隣り合う anim はつなげる
        if segs and segs[-1][2] == "anim" and r[2] == "anim":
            segs[-1][1] = r[1]
        else:
            segs.append(r)
    return a0, n, taus, segs


def bake(E, cdir):
    """要素を焼く → [(出力の開始, 終わり, ファイル)]。できているものは描かない。"""
    import numpy as np
    from PIL import Image
    a0, n, taus, segs = plan_segments(E)
    key, fps = E.key(), E.fps
    off = round(a0 - E.a, 5)
    out = []
    try:
        for i0, i1, kind, sig in segs:
            # 区切りはコマの半分手前に置く(ffmpeg の時刻の丸めで境目のコマが抜けない)
            # 最初も同じ(E.a のままだとコマの格子からずれ、重ねる時に1コマ先の絵が出ることがある。a0 - 半コマ〜E.a にコマは無い)
            t0 = a0 + (i0 - 0.5) / fps
            t1 = E.b if i1 == n else a0 + (i1 - 0.5) / fps
            if kind == "still":
                h = hashlib.sha1(f"{key}|{sig}".encode()).hexdigest()[:16]
                f = cdir / f"{h}.png"
                if not f.exists():
                    tmp = _tmp(f)
                    Image.fromarray(E.render(taus[i0])).save(tmp, compress_level=1)
                    tmp.replace(f)
            else:
                h = hashlib.sha1(f"{key}|{i0}-{i1}|{off}".encode()).hexdigest()[:16]
                f = cdir / (f"{h}.mp4" if E.solid else f"{h}.mov")
                if not f.exists():
                    (_write_mp4 if E.solid else _write_mov)(f, (E.render(taus[i]) for i in range(i0, i1)), E.W, E.H, fps)
            out.append((t0, t1, f))
    finally:
        E.close()
    return out


def joint(plan, o):
    """出力時刻 o の継ぎ目の (前の元素材の終わり, 後ろの元素材の頭)(元素材の時刻)。空白・端は None。継ぎ目でなければ (x, x)"""
    acc, A, B = 0.0, None, None
    for pc in plan.pieces:
        L = pc[1] if pc[0] == "gap" else pc[2] - pc[1]
        if pc[0] == "src":
            if abs(acc + L - o) < 1e-3:
                A = pc[2]
            elif abs(acc - o) < 1e-3:
                B = pc[1]
            elif acc < o < acc + L:
                return (pc[1] + o - acc,) * 2
        acc += L
    return (A, B)


def transitions(plan):
    """トランジションの一覧 → [(継ぎ目の出力時刻, 名前, P, se)]"""
    d, tm = plan.proj.data, plan.tm
    out = []

    def add(o, spec):
        name = spec["type"] if isinstance(spec, dict) else spec
        if name not in REG["trans"]:
            return
        P = params("trans", name, spec.get("params") if isinstance(spec, dict) else None)
        if isinstance(spec, dict) and spec.get("dur") is not None:
            P["dur"] = float(spec["dur"])
        out.append((o, name, P, spec.get("se") if isinstance(spec, dict) else None))

    for t in d.get("transitions") or []:
        if t.get("gap") is not None:
            g = tm.gap_at(parse_time(t["gap"]))
            if not g:
                continue
            sides = t.get("sides", "both")
            if sides in ("both", "in"):
                add(g[0], t)
            if sides in ("both", "out"):
                add(g[0] + g[1], t)
        elif t.get("at") is not None:
            a = parse_time(t["at"])
            add(tm.out(a) if d["timebase"] == "source" else a, t)
    n_own = len(out)
    cf = d.get("cut_fx") or {}
    cv = cf.get("video")
    if isinstance(cv, str) and cf.get("dur") is not None:
        cv = {"type": cv, "dur": cf["dur"]}
    if cv and (cv["type"] if isinstance(cv, dict) else cv) in set(REG["trans"]) - {"dip-black", "dip-white"}:  # dip は今までどおり render.py
        acc, prev = 0.0, None
        for pc in plan.pieces:
            if prev == "src" and pc[0] == "src" and not any(abs(x[0] - acc) < 1e-3 for x in out[:n_own]):  # transitions に書いた継ぎ目はそちら
                add(acc, cv)
            acc += pc[1] if pc[0] == "gap" else pc[2] - pc[1]
            prev = pc[0]
    return out


def extra_sfx(plan):
    """トランジションの効果音(出力上の時刻) → [(a, b, it)]"""
    res = []
    for o, name, P, se in transitions(plan):
        if not se or not se.get("file"):
            continue
        dur = float(P["dur"])
        a = max(0.0, o - dur + float(se.get("offset", 0)))
        b = min(plan.total, a + float(se.get("len", dur)))
        if b > a:
            res.append((a, b, {"file": se["file"], "start": a, "end": b, "in": parse_time(se.get("in", 0)),
                               "gain_db": float(se.get("gain_db", 0)), "kind": "fx-se"}))
    return res


def matte_map(data):
    """マット(matte)の対応 → ({id(要素): (マットに使う要素 or None, mode)}, {隠す要素の id}, {"id": 要素})。
    matte は {"src": "id", "mode": "alpha|luma|alpha-inv|luma-inv"} か、モードだけの文字列(= リストで次の要素をマットに使う)"""
    ovs, tels = data.get("overlays") or [], data.get("telops") or []
    ids = {x["id"]: x for x in ovs + tels if isinstance(x, dict) and x.get("id")}
    out = {}
    for lst in (ovs, tels):
        for i, it in enumerate(lst):
            m = it.get("matte")
            if not m:
                continue
            if isinstance(m, str):
                out[id(it)] = (lst[i + 1] if i + 1 < len(lst) else None, m)
            else:
                out[id(it)] = (ids.get(m.get("src")), m.get("mode", "alpha"))
    return out, {id(s) for s, _ in out.values() if s is not None}, ids


def v1_fx_groups(plan):
    """v1_fx の揺れ以外(end が必要)→ [(出力の a, b, it)]。時間が重なるものは1つにまとめる。
    it = {"kind": "_v1", "fx": [...(start/end はまとめた頭からの秒)], "_alpha": 範囲だけ変えるものだけか}"""
    d = plan.proj.data
    lst = []
    for f in d.get("v1_fx") or []:
        if f.get("type") == "shake" or f.get("type") not in REG["fx"] or f.get("end") is None:
            continue
        a, b = parse_time(f.get("start", 0)), parse_time(f["end"])
        r = plan.tm.out_range(a, b) if d["timebase"] == "source" else (a, min(b, plan.total))
        if r and r[1] > r[0]:
            lst.append([r[0], r[1], [(r[0], r[1], f)]])
    lst.sort(key=lambda x: x[0])
    groups = []
    for g in lst:
        if groups and g[0] < groups[-1][1] - 1e-6:
            groups[-1][1] = max(groups[-1][1], g[1])
            groups[-1][2] += g[2]
        else:
            groups.append(g)
    out = []
    for a, b, fs in groups:
        fx = [{**f, "start": round(o - a, 4), "end": round(e - a, 4)} for o, e, f in fs]
        out.append((a, b, {"kind": "_v1", "fx": fx, "track": 1, "_z": 1, "_alpha": all(REG["fx"][f["type"]].get("region") for f in fx)}))
    return out


def expand(plan, ovs, when=None, jobs=None):
    """差し込み(出力時刻に換算済みの [(a,b,it)])のうち、ここで描く要素を焼いた動画/静止画に置き換え、背景のエフェクトとトランジションを足す。
    when=(lo,hi) ならその範囲に出るものだけ焼く(vedit frame)。結果は plan に覚える。"""
    from concurrent.futures import ThreadPoolExecutor
    memo = plan.__dict__.setdefault("_fx_memo", {})
    mk = (when, bool(getattr(plan, "draft", False)))
    if mk in memo:
        return memo[mk]
    cdir = plan.proj.cache_dir / "fx"
    cdir.mkdir(exist_ok=True)
    data = plan.proj.data
    hit = (lambda a, b: True) if when is None else (lambda a, b: a <= when[1] and b > when[0])
    mattes, hidden, ids = matte_map(data)
    tels = data.get("telops") or []

    def ref(src):
        """参照された要素(マット・親)を描く Elem(焼く要素とは別に作る。並列に焼くので共有しない)"""
        if src is None:
            return None
        it = telop_norm(src) if any(src is t for t in tels) else src
        r = plan.conv([src])
        return Elem(plan, it, r[0][0], r[0][1]) if r else None

    v1g = v1_fx_groups(plan) if "v1" not in plan.hide else []
    work = []  # (並び順, Elem)
    res = []
    shown = []  # ここまでに出した要素(下にあるもの)
    for a, b, it in list(ovs) + telop_items(plan):
        oid = it.get("_oid", id(it))
        if oid in hidden:
            continue
        if it.get("_telop") or needs_bake(it):
            if hit(a, b):
                E = Elem(plan, it, a, b)
                if oid in mattes:
                    src, mode = mattes[oid]
                    E.matte = (ref(src), mode)
                    E.deps.append(["matte", mode, src, E.matte[0] and (E.matte[0].a, E.matte[0].b, E.matte[0].key())])
                p, seen = ids.get(it.get("parent")), set()
                while p is not None and id(p) not in seen:
                    seen.add(id(p))
                    PE = ref(p)
                    if PE:
                        E.parents.append(PE)
                        E.deps.append(["parent", PE.a, PE.b, PE.key()])
                    p = ids.get(p.get("parent"))
                if E.special == "adjust" or E.blend != "normal":
                    below = [x for x in shown if x[0] < b and x[1] > a]
                    E.below = _Below(plan, below, [g for g in v1g if g[0] < b and g[1] > a], E.W, E.H)
                    E.deps.append(["below", [(round(x[0], 4), round(x[1], 4), x[2]) for x in below]])
                    for x in below:
                        if x[2].get("file"):
                            f = plan.proj.resolve(x[2]["file"])
                            if f.exists():
                                E.deps.append([str(f), f.stat().st_mtime_ns])
                E.solid = (E.special == "adjust" and not E.mask and not E.matte and not E.anims and not E.parents
                           and "opacity" not in E.keys)
                work.append((len(res), E))
                res.append(None)
            shown.append((a, b, it))
            continue
        shown.append((a, b, it))
        res.append((a, b, it))
    for a, b, it in v1g:  # 背景(V1)のエフェクト: V1 のすぐ上(トランジションの下)
        if hit(a, b):
            work.append((len(res), Elem(plan, it, a, b)))
            res.append(None)
    if "fx" not in plan.hide:
        for o, name, P, se in transitions(plan):
            dur = float(P["dur"])
            a, b = max(0.0, o - dur), min(plan.total, o + dur)
            if b > a and hit(a, b):
                src = REG["trans"][name].get("src")  # 前後の映像を使うものは V1 のすぐ上(差し込みの下)
                E = Elem(plan, {"kind": "_trans", "track": 1, "_z": 1} if src else {"kind": "_trans", "track": TOP_TRACK}, a, b,
                         trans=(name, P, o, joint(plan, o)))
                work.append((len(res), E))
                res.append(None)
    if work:
        with ThreadPoolExecutor(jobs or min(8, max(1, len(work)))) as ex:
            done = list(ex.map(lambda w: _bake_trans(w[1], cdir) if w[1].trans else bake(w[1], cdir), work))
        for (i, E), segs in zip(work, done):
            src = E.it
            z = {"_z": src["_z"]} if src.get("_z") else {}
            res[i] = [(t0, t1, {"file": str(f), "pos": "full", "track": src.get("track", 2), "_fx": True, **z}) for t0, t1, f in segs]
    flat = []
    for r in res:
        if r is None:
            continue
        flat.extend(r if isinstance(r, list) else [r])
    flat.sort(key=lambda x: x[2].get("_z") or max(2, round(float(x[2].get("track") or 2))))  # トラック順(同じトラックは元の並び)
    memo[mk] = flat
    return flat


def _bake_trans(E, cdir):
    """トランジション: τ は継ぎ目からの秒"""
    o = E.trans[2]
    orig_state = E.state
    E.state = lambda tau: (St(), [])
    E.sig = lambda tau: None
    content = E.content
    E.content = lambda tau: content(tau + E.a - o)
    try:
        return bake(E, cdir)
    finally:
        E.state, E.content = orig_state, content


# ---------------------------------------------------------------- V1(背景)のエフェクト
def v1_items(plan):
    """v1_fx の揺れ → [(出力の開始, 終わり, P)]"""
    d, out = plan.proj.data, []
    conv = lambda x: plan.tm.out(parse_time(x)) if d["timebase"] == "source" else parse_time(x)
    for f in d.get("v1_fx") or []:
        if f.get("type") != "shake":
            continue
        P = params("fx", "shake", f)
        o = conv(f["start"])
        e = o + float(P["dur"])
        if f.get("end") is not None:
            e = min(e, conv(f["end"]))
        if e > o:
            out.append((o, e, P))
    return out


def v1_zoom(plan, items):
    """揺らしても画面の端が見えないだけ拡大する率(全部の揺れで共通)"""
    k = plan.H / 1080.0
    s = 1.0
    for _, _, P in items:
        ax, ay = pair(P["amp"])
        s = max(s, 1 + 2 * (abs(ax) * k + 1) / plan.W, 1 + 2 * (abs(ay) * k + 1) / plan.H)
    return s


def v1_graph(plan, g, cur, win=None, at=None):
    """v1_fx(背景の揺れ)を ffmpeg のフィルタで足す。戻り値: 新しいラベル。
    元の映像を split して、揺らした(少し拡大して切り抜く位置をずらした)映像を揺れている間だけ上に重ねる。
    (trim で揺れの区間だけ切ると、overlay が後ろの区間を待つ間に元の映像が溜まってメモリを食うので、全コマ流して enable で出し分ける)"""
    items = v1_items(plan)
    if win:
        items = [x for x in items if x[1] > win[0] and x[0] < win[1]]
    if at is not None:
        items = [x for x in items if x[0] <= at < x[1]]
    if not items:
        return cur
    k = plan.H / 1080.0
    s = v1_zoom(plan, items)
    cw, ch = int(plan.W / s / 2) * 2, int(plan.H / s / 2) * 2
    xs, ys, en = [], [], []
    for o, e, P in items:
        ex, ey = shake_expr(P, o, k)
        w = f"between(t,{o:.4f},{e:.4f})"
        xs.append(f"if({w},{ex},0)")
        ys.append(f"if({w},{ey},0)")
        en.append(w)
    # 揺れの量は出力の px。切り抜きの中では 1/s 倍
    g.append(f"[{cur}]split[v1a][v1b]")
    g.append(f"[v1b]crop={cw}:{ch}:'(iw-{cw})/2-({'+'.join(xs)})/{s:.6f}':'(ih-{ch})/2-({'+'.join(ys)})/{s:.6f}',"
             f"scale={plan.W}:{plan.H},setsar=1[v1c]")
    g.append(f"[v1a][v1c]overlay=0:0:enable='{'+'.join(en)}'[v1s]")
    return "v1s"


def v1_motion(plan):
    """GUI のプレビュー用: 背景の揺れをコマごとの (dx, dy) の表に(同じ shake_xy から)。s は拡大率"""
    items = v1_items(plan)
    s = v1_zoom(plan, items)
    k = plan.H / 1080.0
    out = []
    for o, e, P in items:
        n = max(1, int(round((e - o) * plan.fps)))
        xy = [shake_xy(i / plan.fps, P, k) for i in range(n)]
        out.append({"a": o, "b": e, "fps": plan.fps, "W": plan.W, "H": plan.H, "s": s,
                    "dx": [round(x, 2) for x, _ in xy], "dy": [round(y, 2) for _, y in xy]})
    return out


# ---------------------------------------------------------------- 検査
MATTE_MODES = ("alpha", "luma", "alpha-inv", "luma-inv")
MASK_SHAPES = ("rect", "ellipse", "polygon")
MASK_MODES = ("add", "subtract", "intersect")


def validate(d, proj=None):
    """エフェクト関係の誤り(知らない名前・パラメータ・キーフレームの時刻の逆順・式の書き間違い・id の参照先が無い 等)"""
    from . import expr
    errs = []
    known_anim = set(REG["anim"]) | {"none"}
    ovs, tels = d.get("overlays") or [], d.get("telops") or []
    ids = {}
    for kind, lst in (("overlays", ovs), ("telops", tels)):
        for i, it in enumerate(lst):
            if isinstance(it, dict) and it.get("id") is not None:
                if it["id"] in ids:
                    errs.append(f"{kind}[{i}]: id {it['id']!r} が重複しています({ids[it['id']][0]} と同じ)")
                else:
                    ids[it["id"]] = (f"{kind}[{i}]", it)

    def chk_ease(w, e):
        try:
            ease_fn(e)
        except (ValueError, TypeError) as ex:
            errs.append(f"{w}: {ex}")

    def chk_val(w, v):
        """キーフレーム・式も可の値"""
        if isinstance(v, str) and v.startswith("="):
            v = {"expr": v[1:]}
        if isinstance(v, dict) and "expr" in v:
            try:
                expr.compile_expr(str(v["expr"]))
            except Exception as ex:  # noqa: BLE001
                errs.append(f"{w}: 式の誤り({ex})")
            return
        if isinstance(v, dict) and "keys" in v:
            ks = v["keys"]
            if not isinstance(ks, list) or not ks:
                errs.append(f"{w}.keys: [[秒, 値, イージング?], ...] の並びにしてください")
                return
            prev = None
            for j, k in enumerate(ks):
                if not isinstance(k, list) or len(k) < 2:
                    errs.append(f"{w}.keys[{j}]: [秒, 値, イージング?] にしてください")
                    return
                try:
                    tk = float(k[0])
                except (TypeError, ValueError):
                    errs.append(f"{w}.keys[{j}]: 時刻 {k[0]!r} が数値ではありません")
                    return
                if prev is not None and tk < prev:
                    errs.append(f"{w}.keys[{j}]: キーフレームの時刻が逆順です({prev} → {tk})")
                prev = tk
                if len(k) > 2 and k[2]:
                    chk_ease(f"{w}.keys[{j}]", k[2])
            if v.get("loop") not in (None, False, "cycle", "pingpong", True):
                errs.append(f"{w}.loop: cycle か pingpong です")
            return
        if isinstance(v, dict):
            for k, x in v.items():
                chk_val(f"{w}.{k}", x)
        elif isinstance(v, list):
            for j, x in enumerate(v):
                if isinstance(x, (dict, str)):
                    chk_val(f"{w}[{j}]", x)

    def chk_params(where, cat, name, given, skip=()):
        spec = REG[cat][name]
        allowed = set(spec["params"]) | {"type", "start", "end"} | set(skip) | ({"region"} if spec.get("kind") == "pixel" else set())
        for k in given:
            if k not in allowed:
                errs.append(f"{where}: {name} に {k!r} というパラメータはありません(使えるもの: {', '.join(sorted(spec['params']))})")
        if "ease" in given and not is_dyn(given["ease"]):
            chk_ease(where, given["ease"])
        for k, v in given.items():
            if k not in ("type", "start", "end") and has_dyn(v):
                chk_val(f"{where}.{k}", v)

    def chk_anim(w, side, v, telop):
        if v is None:
            return
        name = v.get("type") if isinstance(v, dict) else v
        if name not in known_anim:
            errs.append(f"{w}.{side}: {name!r} という動きはありません(使えるもの: {', '.join(sorted(known_anim))})")
            return
        if name != "none" and REG["anim"][name].get("telop_only") and not telop:
            errs.append(f"{w}.{side}: {name} はテロップ(telops)専用です")
        if isinstance(v, dict):
            chk_params(f"{w}.{side}", "anim", name, v)

    def chk_fx(w, lst):
        for j, f in enumerate(lst or []):
            if f.get("type") not in REG["fx"]:
                errs.append(f"{w}.fx[{j}]: {f.get('type')!r} というエフェクトはありません(使えるもの: {', '.join(sorted(REG['fx']))})")
                continue
            chk_params(f"{w}.fx[{j}]", "fx", f["type"], f)
            if f.get("end") is not None and parse_time(f["end"]) <= parse_time(f.get("start", 0)):
                errs.append(f"{w}.fx[{j}]: end が start 以前です")

    def chk_mask(w, m):
        lst = [m] if isinstance(m, dict) else m
        if not isinstance(lst, list):
            errs.append(f"{w}.mask: {{shape, rect|points, feather?, invert?, mode?}} かその並びにしてください")
            return
        for j, s in enumerate(lst):
            ww = f"{w}.mask[{j}]"
            if not isinstance(s, dict):
                errs.append(f"{ww}: {{shape, rect|points, ...}} にしてください")
                continue
            shape = s.get("shape") or ("rect" if s.get("rect") else "polygon")
            if shape not in MASK_SHAPES:
                errs.append(f"{ww}.shape: {shape!r} は使えません(使えるもの: {', '.join(MASK_SHAPES)})")
            if s.get("mode", "add") not in MASK_MODES:
                errs.append(f"{ww}.mode: {s.get('mode')!r} は使えません(使えるもの: {', '.join(MASK_MODES)})")
            if shape in ("rect", "ellipse") and s.get("rect") is not None and not is_dyn(s["rect"]) and (
                    not isinstance(s["rect"], list) or len(s["rect"]) != 4):
                errs.append(f"{ww}.rect: [x, y, 幅, 高さ](1080p 基準 px)にしてください")
            if shape == "polygon" and not is_dyn(s.get("points")) and len(s.get("points") or []) < 3:
                errs.append(f"{ww}.points: 3点以上の [[x, y], ...] が必要です")
            for k in s:
                if k not in ("shape", "rect", "points", "feather", "invert", "mode"):
                    errs.append(f"{ww}: {k!r} は使えません(shape / rect / points / feather / invert / mode)")
            chk_val(ww, {k: v for k, v in s.items() if k in ("rect", "points", "feather")})

    def chk_keys(w, it):
        ks = it.get("keys")
        if ks is None:
            return
        if not isinstance(ks, dict):
            errs.append(f"{w}.keys: {{\"プロパティ\": 値・キーフレーム・式}} にしてください")
            return
        fxl = it.get("fx") or []
        for k, v in ks.items():
            if "." in k:
                tgt, prm = k.split(".", 1)
                hit = [f for i, f in enumerate(fxl) if tgt in (f.get("type"), f"fx[{i}]")]
                if not hit:
                    errs.append(f"{w}.keys.{k}: {tgt!r} というエフェクトがこの要素の fx にありません")
                elif hit[0].get("type") in REG["fx"] and prm not in REG["fx"][hit[0]["type"]]["params"] and prm != "region":
                    errs.append(f"{w}.keys.{k}: {hit[0]['type']} に {prm!r} というパラメータはありません")
            elif k not in KEY_PROPS:
                errs.append(f"{w}.keys: {k!r} は動かせません(使えるもの: {', '.join(KEY_PROPS)}、fx のパラメータは \"blur.radius\" のように)")
            if not isinstance(v, (int, float)) or isinstance(v, bool):
                if not is_dyn(v):
                    errs.append(f"{w}.keys.{k}: 数値・\"=式\"・{{\"keys\": [[秒, 値, イージング?], ...]}}・{{\"expr\": \"式\"}} のどれかにしてください")
                else:
                    chk_val(f"{w}.keys.{k}", v)

    def chk_common(w, it, lst, i, telop):
        """overlays・telops・comp の layers 共通"""
        for side in ("in_anim", "out_anim"):
            chk_anim(w, side, it.get(side), telop)
        chk_fx(w, it.get("fx"))
        chk_keys(w, it)
        if it.get("mask") is not None:
            chk_mask(w, it["mask"])
        if it.get("blend") not in (None, "normal") and it.get("blend") not in BLEND:
            errs.append(f"{w}.blend: {it['blend']!r} は使えません(使えるもの: {', '.join(BLEND)})")
        m = it.get("matte")
        if m:
            mode = m if isinstance(m, str) else m.get("mode", "alpha") if isinstance(m, dict) else None
            if mode not in MATTE_MODES:
                errs.append(f"{w}.matte: モードは {' / '.join(MATTE_MODES)} です")
            if isinstance(m, str) and lst is not None and i + 1 >= len(lst):
                errs.append(f"{w}.matte: モードだけ書いた時は、同じ並びの次の要素をマットに使います(次の要素がありません)")
            if isinstance(m, dict) and m.get("src") not in ids:
                errs.append(f"{w}.matte.src: id {m.get('src')!r} の要素がありません")
        p = it.get("parent")
        if p is not None:
            if p not in ids:
                errs.append(f"{w}.parent: id {p!r} の要素がありません")
            else:
                seen, q = {id(it)}, ids[p][1]
                while q is not None:
                    if id(q) in seen:
                        errs.append(f"{w}.parent: 親子が輪になっています")
                        break
                    seen.add(id(q))
                    q = ids.get(q.get("parent"), (None, None))[1]
        an = it.get("animator")
        if an:
            if not telop:
                errs.append(f"{w}.animator: 文字アニメーターはテロップ(telops・comp の text)専用です")
            for j, a in enumerate([an] if isinstance(an, dict) else an if isinstance(an, list) else [None]):
                if not isinstance(a, dict) or a.get("type") not in REG["text"]:
                    errs.append(f"{w}.animator[{j}]: {{\"type\": {' | '.join(REG['text'])}, ...}} にしてください")
                    continue
                chk_params(f"{w}.animator[{j}]", "text", a["type"], a)
                if a.get("order", "forward") not in ("forward", "backward", "center", "random"):
                    errs.append(f"{w}.animator[{j}].order: forward / backward / center / random のどれかです")

    def chk_overlay(w, it, lst, i):
        if it.get("preset"):
            return  # presets.validate で見る
        kind = it.get("kind")
        if not it.get("file") and kind not in REG["gen"] and kind not in ("adjust", "comp"):
            errs.append(f"{w}: file が無い要素は kind に生成要素の名前({', '.join(REG['gen'])})か adjust / comp が必要です")
        if not it.get("file") and kind in REG["gen"]:
            chk_params(w, "gen", kind, it.get("params") or {})
        if not it.get("file") and kind == "adjust" and not it.get("fx"):
            errs.append(f"{w}: 調整レイヤー(kind: adjust)には fx が必要です")
        if not it.get("file") and kind == "comp":
            if it.get("project"):
                if proj is not None and not proj.resolve(it["project"]).is_file():
                    errs.append(f"{w}.project: ファイルがありません: {it['project']!r}")
            elif not isinstance(it.get("layers"), list) or not it["layers"]:
                errs.append(f"{w}: プリコンポーズ(kind: comp)には layers か project が必要です")
            for j, L in enumerate(it.get("layers") or []):
                lw = f"{w}.layers[{j}]"
                if L.get("text") is not None and not L.get("file") and not L.get("kind"):
                    chk_common(lw, L, it["layers"], j, True)
                else:
                    chk_overlay(lw, L, it["layers"], j)
                if L.get("end") is not None and parse_time(L["end"]) <= parse_time(L.get("start", 0)):
                    errs.append(f"{lw}: end が start 以前です")
        chk_common(w, it, lst, i, False)

    for i, it in enumerate(ovs):
        chk_overlay(f"overlays[{i}]", it, ovs, i)
    for i, t in enumerate(tels):
        w = f"telops[{i}]"
        if t.get("preset"):
            continue
        a = t.get("anim")
        if isinstance(a, dict) or a not in LEGACY_TELOP_ANIM:
            chk_anim(w, "anim", a, True)
        chk_common(w, t, tels, i, True)
    for j, f in enumerate(d.get("v1_fx") or []):
        w = f"v1_fx[{j}]"
        if f.get("type") not in REG["fx"]:
            errs.append(f"{w}: {f.get('type')!r} というエフェクトはありません(使えるもの: {', '.join(sorted(REG['fx']))})")
            continue
        if "start" not in f:
            errs.append(f"{w}: start(かけ始める時刻)が必要です")
        if f["type"] != "shake" and f.get("end") is None:
            errs.append(f"{w}: 背景の {f['type']} には end(かけ終わる時刻)が必要です(省略できるのは shake だけ)")
        if f["type"] in ("zoom", "drift"):
            errs.append(f"{w}: 背景(V1)には {f['type']} は使えません(調整レイヤーか、背景を差し込みとして置いてください)")
        chk_params(w, "fx", f["type"], f)
        if f.get("end") is not None and "start" in f and parse_time(f["end"]) <= parse_time(f["start"]):
            errs.append(f"{w}: end が start 以前です")
    for j, t in enumerate(d.get("transitions") or []):
        if t.get("type") not in REG["trans"]:
            errs.append(f"transitions[{j}]: {t.get('type')!r} というトランジションはありません(使えるもの: {', '.join(sorted(REG['trans']))})")
        else:
            chk_params(f"transitions[{j}].params", "trans", t["type"], t.get("params") or {})
        if (t.get("at") is None) == (t.get("gap") is None):
            errs.append(f"transitions[{j}]: at(カットの継ぎ目の元素材の時刻)か gap(空白の位置)のどちらか1つを書いてください")
        if t.get("sides", "both") not in ("both", "in", "out"):
            errs.append(f"transitions[{j}].sides: both / in / out のどれかです")
        if t.get("se") and not t["se"].get("file"):
            errs.append(f"transitions[{j}].se: file が必要です")
    cv = (d.get("cut_fx") or {}).get("video")
    if cv and (cv["type"] if isinstance(cv, dict) else cv) not in set(REG["trans"]) | {"none", "dip-black", "dip-white"}:
        errs.append(f"cut_fx.video: {cv!r} は使えません")
    errs += presets.validate(d, proj)
    return errs


def catalog():
    """vedit fx list の中身"""
    out = {}
    for cat, tab in REG.items():
        out[cat] = {n: {"doc": s["doc"], "params": s["params"]} for n, s in tab.items()}
    out["preset"] = presets.catalog()
    return out


# ---------------------------------------------------------------- GUI のプレビュー
PV_W = 960  # プレビュー用の動画の幅(モニターに出すだけなので小さく)


def _pv_webm(mov):
    """焼いた .mov(qtrle)・.mp4(不透明)をブラウザで再生できる透明つき webm(VP9)に。キャッシュする"""
    out = mov.parent / "pv" / (mov.stem + ".webm")
    if out.exists():
        return out
    out.parent.mkdir(exist_ok=True)
    tmp = _tmp(out)
    subprocess.run([ff.find("ffmpeg"), "-y", "-v", "error", "-i", str(mov), "-vf", f"scale={PV_W}:-2", "-c:v", "libvpx-vp9",
                    "-pix_fmt", "yuva420p", "-deadline", "realtime", "-cpu-used", "8", "-b:v", "0", "-crf", "34", "-auto-alt-ref", "0",
                    str(tmp)], check=True, creationflags=ff.NOWIN)
    try:
        tmp.replace(out)
    except OSError:  # 同時に作った方がもう置いて、ブラウザが開いている
        if not out.exists():
            raise
        tmp.unlink(missing_ok=True)
    return out


def preview(proj, data):
    """GUI 用: data(GUI が持っている編集中のプロジェクト)のエフェクトを焼き、モニターに出すものを返す。
    items: 焼いた絵(出力上の a〜b 秒・トラック・ファイル)、replaced: 焼いた絵に置き換わる overlays の番号(元の要素は透明にして選択だけできる)、
    v1: 背景の揺れの表(v1_motion)"""
    import copy
    from concurrent.futures import ThreadPoolExecutor
    from . import render
    p2 = copy.copy(proj)
    p2.data = data
    plan = render.Plan(p2)
    ovs = plan.conv(plan.overlays())
    out = expand(plan, ovs)
    idx = {id(o): i for i, o in enumerate(data.get("overlays") or [])}
    hidden = matte_map(data)[1]  # マットに使う要素も(書き出しでは出ないので)透明に
    replaced = sorted({idx[id(it)] for _, _, it in ovs if (needs_bake(it) or id(it) in hidden) and id(it) in idx} |
                      {i for i, o in enumerate(data.get("overlays") or []) if o.get("preset")})  # プリセットは展開したものを焼いて出す
    replaced_t = [i for i, t in enumerate(data.get("telops") or []) if needs_bake_telop(t) or id(t) in hidden or t.get("preset")]
    fx_items = [(a, b, it) for a, b, it in out if it.get("_fx")]
    movs = [Path(it["file"]) for _, _, it in fx_items if it["file"].endswith((".mov", ".mp4"))]
    with ThreadPoolExecutor(4) as ex:
        webm = dict(zip(movs, ex.map(_pv_webm, movs)))
    items = []
    for a, b, it in fx_items:
        f = Path(it["file"])
        items.append({"a": round(a, 4), "b": round(b, 4), "track": it.get("track", 2), "file": str(webm.get(f, f)), "img": f.suffix == ".png"})
    se = [{"file": it["file"], "at": round(a, 4), "len": round(b - a, 4), "in": float(it.get("in", 0)), "gain_db": float(it.get("gain_db", 0))}
          for a, b, it in ([] if "se" in plan.mute else extra_sfx(plan) + [x for x in plan.sfx() if x[2].get("_preset") or x[2].get("_match")])]
    return {"items": items, "replaced": replaced, "replaced_t": replaced_t, "v1": v1_motion(plan), "se": se}


def uses_fx(data):
    """エフェクト関係のキーがあるか(無ければ GUI はプレビュー用の焼き付けを頼まない)"""
    return bool(data.get("v1_fx") or data.get("transitions") or
                (isinstance((data.get("cut_fx") or {}).get("video"), (str, dict)) and
                 ((data["cut_fx"]["video"]["type"] if isinstance(data["cut_fx"]["video"], dict) else data["cut_fx"]["video"]) in REG["trans"])) or
                any(needs_bake(it) for it in data.get("overlays") or []) or any(needs_bake_telop(t) for t in data.get("telops") or []) or
                presets.has_work(data))


# ---------------------------------------------------------------- vedit fx preview(見本)

def catalog_md():
    """catalog() を AGENTS.md の表(Markdown)に。名前・既定を変えたらこれで作り直して貼り替える"""
    title = {"anim": "出入り(in_anim / out_anim)", "fx": "エフェクト(fx / v1_fx)", "gen": "生成要素(overlays の kind)",
             "trans": "トランジション(transitions / cut_fx.video)", "text": "文字アニメーター(telops の animator)",
             "preset": "プリセット(overlays / telops の preset)"}
    out = []
    for cat, tab in catalog().items():
        out.append(f"\n#### {title.get(cat, cat)}\n| 名前 | 内容 | パラメータ(既定) |\n|---|---|---|")
        for n, s in tab.items():
            P = ", ".join(f"`{k}`={json.dumps(v, ensure_ascii=False)}" for k, v in s["params"].items() if not k.startswith("_"))
            if cat == "preset":
                P = P.replace("se/", "")
            out.append(f"| `{n}` | {s['doc'].replace(chr(10), ' ').replace('|', '/')} | {P} |")
    return "\n".join(out)

def resolve_name(name):
    """'pop' / 'anim:pop' / 'preset:gloom' → (種類, 名前)。同じ名前が複数の種類にあれば anim → fx → gen → trans → text → preset の順に選ぶ"""
    tabs = {**REG, "preset": presets.load()}
    if ":" in name:
        cat, n = name.split(":", 1)
        if cat in tabs and n in tabs[cat]:
            return cat, n
    else:
        for cat in ("anim", "fx", "gen", "trans", "text", "preset"):
            if name in tabs[cat]:
                return cat, name
    raise SystemExit(f"知らない名前です: {name!r}(vedit fx list で一覧)")


def _sample_assets(d):
    """見本用の背景動画(testsrc2・5秒・無音)と差し込み画像(赤い板に SAMPLE)を一度だけ作る"""
    from PIL import Image, ImageDraw, ImageFont
    src, img = d / "src.mp4", d / "sample.png"
    if not src.exists():
        subprocess.run([ff.find("ffmpeg"), "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=s=1920x1080:r=30:d=5",
                        "-f", "lavfi", "-i", "anullsrc=r=48000:cl=stereo", "-t", "5", "-c:v", "libx264", "-g", "15",
                        "-pix_fmt", "yuv420p", "-c:a", "aac", str(src)], check=True, creationflags=ff.NOWIN)
    if not img.exists():
        im = Image.new("RGBA", (800, 300), (0, 0, 0, 0))
        dr = ImageDraw.Draw(im)
        dr.rounded_rectangle((0, 0, 799, 299), 40, fill=(200, 20, 27, 255), outline=(255, 255, 255, 255), width=10)
        try:
            font = ImageFont.truetype("C:/Windows/Fonts/HGRSGU.TTC", 150)
        except OSError:
            font = ImageFont.load_default(150)
        dr.text((400, 150), "SAMPLE", font=font, fill="white", anchor="mm", stroke_width=6, stroke_fill="black")
        im.save(img)


def sample_project(name, user, frames=10):
    """見本のプロジェクト(一時フォルダ)を作り、(Project, 見るコマの時刻(カット後), '種類:名前') を返す"""
    import tempfile
    from .project import Project
    cat, n = resolve_name(name)
    d = Path(tempfile.gettempdir()) / "vedit_fxpv"
    d.mkdir(exist_ok=True)
    _sample_assets(d)
    path = d / "sample.vedit.json"
    proj = Project.create(path, "src.mp4")
    data = proj.data
    P = params(cat, n, user) if cat in REG else {}
    span = float(P.get("dur") or 1.5)
    ov = {"file": "sample.png", "pos": "center", "start": 1.0, "end": 4.0, "track": 2}
    if cat == "anim" and REG[cat][n].get("telop_only"):
        text = "1文字ずつ出るテロップ"
        data["telops"] = [{"start": 1.0, "end": 4.0, "text": text, "in_anim": {"type": n, **user}}]
        ov = None
        t0, t1 = 1.0, 1.0 + float(P["interval"]) * (len(text) + 1)
    elif cat == "anim":
        side = REG[cat][n].get("side", "in")
        ov[side + "_anim"] = {"type": n, **user}
        t0, t1 = (1.0, 1.0 + span + 0.1) if side == "in" else (4.0 - span - 0.1, 4.0 - 1 / 30)
    elif cat == "fx":
        ov["fx"] = [{"type": n, **user}]
        t0, t1 = 1.0, 1.0 + min(span, 2.5)
    elif cat == "text":
        text = "文字が動くテロップ"
        data["telops"] = [{"start": 1.0, "end": 4.0, "text": text, "animator": {"type": n, **user}}]
        ov = None
        t0, t1 = 1.0, 1.0 + float(P["stagger"]) * len(text) + float(P["dur"]) + (0.8 if n in ("wave", "shake") else 0.1)
    elif cat == "preset":
        ov = {"preset": n, "start": 1.0, "end": 3.0, **user}
        if n == "chapter":
            data["gaps"] = [[2.5, 2.0]]
            ov = {"preset": n, "gap_at": 2.5, "start": 0, "end": 2.0, "text": "其の一 見本の章", **user}
            t0, t1 = 2.0, 5.0
        else:
            if n == "champ":
                ov.setdefault("file", "sample.png")
            elif "file" not in ov:
                ov.setdefault("text", "見本")
            t0, t1 = 1.0, 2.0
    elif cat == "gen":
        ov = {"kind": n, "start": 1.0, "end": 1.0 + max(span, 0.2), "track": 2, "params": user}
        t0, t1 = 1.0, 1.0 + max(span, 0.2) + 0.1
    else:
        ov = None
        data["cuts"] = [[2.5, 3.0]]
        data["transitions"] = [{"at": 2.5, "type": n, "params": user}]
        t0, t1 = 2.5 - span - 0.05, 2.5 + span + 0.05
    data["overlays"] = [ov] if ov else []
    proj.save()
    k = max(2, int(frames))
    return proj, [t0 + (t1 - t0) * i / (k - 1) for i in range(k)], f"{cat}:{n}"
