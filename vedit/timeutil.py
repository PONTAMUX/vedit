"""時間の読み書きと、カット後のタイムライン換算。"""
import re


def parse_time(v):
    """秒(数値) / "12.5" / "1:02.5" / "01:02:03.5" を秒(float)にする。"""
    if isinstance(v, (int, float)):
        return float(v)
    s = str(v).strip()
    if re.fullmatch(r"\d+(\.\d+)?", s):
        return float(s)
    parts = s.split(":")
    if not 2 <= len(parts) <= 3:
        raise ValueError(f"時間として読めません: {v!r}")
    sec = 0.0
    for p in parts:
        sec = sec * 60 + float(p)
    return sec


def fmt_time(t):
    t = round(max(0.0, float(t)), 2)  # 先に丸める(59.999 が「0:60.00」にならないように)
    h, rem = divmod(t, 3600)
    m, s = divmod(rem, 60)
    return f"{int(h)}:{int(m):02d}:{s:05.2f}" if h else f"{int(m)}:{s:05.2f}"


class TimeMap:
    """元素材の時間 → カット後の時間 の換算。"""

    MIN_KEEP = 0.02

    def __init__(self, duration, cuts, gaps=()):
        self.duration = float(duration)
        merged = []
        for a, b in sorted((max(0.0, a), min(self.duration, b)) for a, b in cuts):
            if b <= a:
                continue
            if merged and a <= merged[-1][1]:
                merged[-1][1] = max(merged[-1][1], b)
            else:
                merged.append([a, b])
        self.cuts = merged
        keeps, pos = [], 0.0
        for a, b in merged:
            if a - pos >= self.MIN_KEEP:
                keeps.append((pos, a))
            pos = b
        if self.duration - pos >= self.MIN_KEEP:
            keeps.append((pos, self.duration))
        self.keeps = keeps
        # 空白(黒+無音) = 元時間 at の直前に dur 秒を挿入。カット内の at はカットの終わりへ寄せる
        gm = {}
        for at, dur in gaps:
            at, dur = min(max(0.0, float(at)), self.duration), float(dur)
            if dur < self.MIN_KEEP:
                continue
            for a, b in merged:
                if a <= at < b:
                    at = b
            at = round(at, 4)
            gm[at] = gm.get(at, 0.0) + dur
        self.gaps = sorted(gm.items())
        self.total = sum(b - a for a, b in keeps) + sum(d for _, d in self.gaps)

    def pieces(self):
        """出力順の並び: ("src", a, b) / ("gap", 長さ)。"""
        out, gi, G = [], 0, self.gaps
        for a, b in self.keeps:
            while gi < len(G) and G[gi][0] <= a + 1e-6:
                out.append(("gap", G[gi][1])); gi += 1
            # keep の途中に入る空白で区間を割る
            while gi < len(G) and G[gi][0] < b - 1e-6:
                out.append(("src", a, G[gi][0])); a = G[gi][0]
                out.append(("gap", G[gi][1])); gi += 1
            if b - a >= 1e-4:
                out.append(("src", a, b))
        while gi < len(G):
            out.append(("gap", G[gi][1])); gi += 1
        return out

    def gap_at(self, at):
        """元時間 at の空白(カット内ならカットの終わりへ寄せたもの)の (出力上の開始, 長さ)。無ければ None。"""
        at = min(max(0.0, float(at)), self.duration)
        for a, b in self.cuts:
            if a <= at < b:
                at = b
        at = round(at, 4)
        for g, d in self.gaps:
            if abs(g - at) < 1e-3:
                return self.out(g) - d, d
        return None

    def out(self, t):
        """元時間 t までに残る長さ = カット後の時刻。単調で、カット区間では一定。"""
        acc = 0.0
        for a, b in self.keeps:
            if t <= a:
                break
            acc += min(t, b) - a
        return acc + sum(d for at, d in self.gaps if at <= t + 1e-9)

    def out_end(self, t):
        """終わりの時刻の換算。ちょうど t に入る空白は含めない(空白の直前で終わるものが空白の中まで伸びないように)。"""
        return self.out(t) - sum(d for at, d in self.gaps if abs(at - t) <= 1e-6)

    def out_range(self, a, b):
        """範囲を換算。カットで消えて長さ0になれば None。"""
        oa, ob = self.out(a), self.out_end(b)
        return (oa, ob) if ob - oa > 1e-3 else None
