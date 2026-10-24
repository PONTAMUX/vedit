"""自前の De-click(リップノイズ/クリック除去)。numpy のみ。

方式: 2階差分(高域の急変)が「周囲の通常レベル」より大きく跳ねた短い区間をクリックと判定し、
その区間を前後の波形から3次多項式でつなぎ直す。子音(s, t など)は周囲の高域レベルも高いので誤検出しにくい。
"""
import numpy as np

DEFAULTS = {
    "threshold": 8.0,    # 大きいほど検出が減る(周囲の通常レベルの何倍で跳ねたらクリックか)
    "widen_ms": 0.6,     # 検出区間の前後に足す余白
    "max_ms": 8.0,       # これより長く続く跳ねはクリックとみなさず触らない(声・破裂音の保護)
    "window_ms": 60.0,   # 「周囲の通常レベル」を測る幅
}
CONTEXT = 12  # 多項式あてはめに使う前後のサンプル数


def _repair(x, a, b):
    """x[a:b] を前後 CONTEXT サンプルからの3次近似で置き換える(in place)。"""
    lo, hi = max(0, a - CONTEXT), min(len(x), b + CONTEXT)
    idx = np.r_[np.arange(lo, a), np.arange(b, hi)]
    if len(idx) < 6:
        return
    t0 = (a + b) / 2.0
    scale = max(1.0, (hi - lo) / 2.0)
    coef = np.polyfit((idx - t0) / scale, x[idx], 3)
    x[a:b] = np.polyval(coef, (np.arange(a, b) - t0) / scale)


def declick_channel(x, sr, threshold, widen_ms, max_ms, window_ms):
    """1チャンネル(float配列)を処理して (結果, 修復数) を返す。"""
    x = x.astype(np.float64, copy=True)
    if len(x) < 64:
        return x, 0
    d2 = np.abs(np.diff(x, 2, prepend=x[:1], append=x[-1:])[1:-1])
    d2 = np.r_[d2[:1], d2, d2[-1:]]
    win = max(8, int(sr * window_ms / 1000))
    cs0 = np.r_[0.0, np.cumsum(d2)]
    i = np.arange(len(d2))
    lo, hi = np.maximum(0, i - win), np.minimum(len(d2), i + 1 + win)

    def around(cs):
        # 直前の窓と直後の窓のうち高いほうを「通常レベル」とする(音が急に出始める所を誤検出しない)
        left = (cs[i] - cs[lo]) / np.maximum(1, i - lo)
        right = (cs[hi] - cs[i + 1]) / np.maximum(1, hi - (i + 1))
        return np.maximum(left, right) + 1e-6

    # 1回目で大まかなレベルを出し、クリックの跳ねを頭打ちにしてから2回目で正確なレベルを出す
    cap = np.minimum(d2, 3 * around(cs0))
    level = around(np.r_[0.0, np.cumsum(cap)])
    hit = d2 > threshold * level
    hit &= d2 > 1e-4  # 無音域の微小な揺れは対象外

    w = int(sr * widen_ms / 1000)
    if w:
        hit = np.convolve(hit, np.ones(2 * w + 1), mode="same") > 0
    edges = np.flatnonzero(np.diff(np.r_[False, hit, False].astype(np.int8)))
    max_len = int(sr * max_ms / 1000)
    fixed = 0
    for a, b in zip(edges[::2], edges[1::2]):
        if b - a <= max_len:
            _repair(x, a, b)
            fixed += 1
    return x, fixed


def declick(audio, sr, **params):
    """audio: (チャンネル, サンプル) の float 配列。 (結果, 修復数) を返す。"""
    p = {**DEFAULTS, **params}
    out = np.empty_like(audio, dtype=np.float32)
    total = 0
    for ch in range(audio.shape[0]):
        y, n = declick_channel(audio[ch], sr, p["threshold"], p["widen_ms"], p["max_ms"], p["window_ms"])
        out[ch] = y
        total += n
    return out, total
