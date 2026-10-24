"""修正対応: 修正前(先方に見せた版)の控えと、先方の修正点の一覧。<名前>.review.json に置く(project には入れない。書き出しに影響しない)。

- 修正点の時刻は先方が見た動画(修正前の書き出し)の時刻。修正前の project のカットで元素材の時刻に直して持つので、
  カットを足して後ろの時刻がずれても、GUI から正しい所へ飛べる。
- 修正前の動画(書き出した mp4)を GUI の再生ヘッドに合わせて並べて見られるよう、修正前・今の出力上の並び(pieces)を返す。
- 同じ動画を何回も直す時は回ごとに持つ: 一番上が今回 {"created","video","base","points","prev":[前の回, …(古い順)]}。
  今回を一番上に置くので、回を知らない古い vedit でも今回の修正点は読める(prev はそのまま残る)。
  prev の無い形は1回だけ。{"rounds": [...]} の形も読む(書く時に上の形へ直す)。r は回の番号(0 始まり。None = 最新)。
"""
import copy
import json
import re
import time
import unicodedata
from pathlib import Path

from . import ff
from .timeutil import TimeMap, parse_time

LISTS = (("overlays", "差し込み"), ("telops", "テロップ"), ("topics", "トピック"), ("sfx", "効果音"))
_dur_cache = {}


def path_of(proj):
    name = proj.path.name
    stem = name[:-len(".vedit.json")] if name.endswith(".vedit.json") else proj.path.stem
    return proj.path.with_name(stem + ".review.json")


def load(proj):
    f = path_of(proj)
    if not f.exists():
        return None
    rv = json.loads(f.read_text("utf-8"))
    if "rounds" in rv:  # 一時期の形 {"rounds": [...]} → 今回を一番上に
        rs = rv["rounds"]
        rv = {**rs[-1], "prev": rs[:-1]}
    return rv


def _write(proj, rv):
    f = path_of(proj)
    tmp = f.with_name(f.name + ".tmp")
    tmp.write_text(json.dumps(rv, ensure_ascii=False, indent=1), "utf-8")
    tmp.replace(f)


def rounds(rv):
    return list(rv.get("prev") or []) + [rv]


def _round(rv, r=None):
    rs = rounds(rv)
    if r is None:
        return rs[-1]
    r = int(r)
    if not 0 <= r < len(rs):
        raise ValueError(f"修正点{r + 1} はありません(1〜{len(rs)})")
    return rs[r]


def _snap(proj, video):
    v = Path(video) if video else default_video(proj)
    if v and not v.is_absolute():
        v = proj.dir / v
    return {"created": time.strftime("%Y-%m-%d %H:%M"), "video": _rel(proj, v) if v and v.exists() else None,
            "base": copy.deepcopy(proj.data), "points": []}


def _duration(proj, data):
    src = proj.resolve(data["source"])
    key = (str(src), src.stat().st_mtime_ns)
    if key not in _dur_cache:
        _dur_cache[key] = ff.probe(src)["duration"]
    return _dur_cache[key]


def timemap(proj, data):
    cuts = [(parse_time(a), parse_time(b)) for a, b in data.get("cuts") or []]
    gaps = [(parse_time(a), float(d)) for a, d in data.get("gaps") or []]
    return TimeMap(_duration(proj, data), cuts, gaps)


def pieces(tm):
    """出力順の並び [{"o","s","e"} | {"o","gap","at"}](GUI の pieces() と同じ形)。"""
    out, o, gi = [], 0.0, 0
    for p in tm.pieces():
        if p[0] == "gap":
            out.append({"o": round(o, 4), "gap": p[1], "at": tm.gaps[gi][0]})
            o += p[1]
            gi += 1
        else:
            out.append({"o": round(o, 4), "s": p[1], "e": p[2]})
            o += p[2] - p[1]
    return out


def src_of(ps, o):
    """出力上の秒 → (元素材の秒, 空白の位置 or None)。"""
    for p in ps:
        L = p["gap"] if "gap" in p else p["e"] - p["s"]
        if o < p["o"] + L:
            return (p["at"], p["at"]) if "gap" in p else (p["s"] + max(0.0, o - p["o"]), None)
    if not ps:
        return 0.0, None
    p = ps[-1]
    return (p["at"], p["at"]) if "gap" in p else (p["e"], None)


def default_video(proj):
    """修正前の動画: output.file があればそれ、無ければ project と同じ名前の mp4。"""
    cands = []
    if (proj.data.get("output") or {}).get("file"):
        cands.append(proj.resolve(proj.data["output"]["file"]))
    name = path_of(proj).name[:-len(".review.json")]
    cands += [proj.dir / f"{name}.mp4", proj.dir / "out" / f"{name}.mp4"]
    return next((c for c in cands if c.exists()), None)


def _rel(proj, f):
    try:
        return Path(f).resolve().relative_to(proj.dir.resolve()).as_posix()
    except ValueError:
        return Path(f).resolve().as_posix()


def start(proj, video=None, force=False):
    """今の project(ディスクの内容)を修正前として控える。既にあれば force の時だけ作り直す(修正点は消える)。"""
    old = load(proj)
    if old and not force:
        raise ValueError("修正前の控えはもうあります(作り直すなら force)")
    rv = _snap(proj, video)
    _write(proj, rv)
    return rv


def next_round(proj, video=None):
    """次の回: 今の project(先方に見せ直す版)を修正前として控え、修正点の入れ物を足す。前の回はそのまま残る。"""
    rv = load(proj)
    if not rv:
        return start(proj, video=video)
    cur = {k: v for k, v in rv.items() if k != "prev"}
    new = {**_snap(proj, video), "prev": list(rv.get("prev") or []) + [cur]}
    _write(proj, new)
    return new


def norm_line(s):
    return unicodedata.normalize("NFKC", s).strip()


def parse_points(text):
    """「0:07 画像」「0;07　画像」「042: 景気良い」「1:02:03 …」のような行を [(秒, 内容)] に。時刻の無い行は前の点の続き。"""
    out = []
    for raw in text.splitlines():
        s = norm_line(raw)
        if not s:
            continue
        m = re.match(r"^(\d{1,2})\s*[:;.]\s*(\d{1,2})(?:\s*[:;.]\s*(\d{1,2}))?(?!\d)\s*[:;、,]?\s*(.*)$", s)
        if m:
            a, b, c = int(m[1]), int(m[2]), m[3]
            t = a * 3600 + b * 60 + int(c) if c is not None else a * 60 + b
            out.append([t, m[4].strip()])
            continue
        m = re.match(r"^(\d{3,4})\s*[:;、,]?\s*(.*)$", s)  # 「042:」= 0:42(区切りの打ち間違い)
        if m:
            out.append([int(m[1][:-2]) * 60 + int(m[1][-2:]), m[2].strip()])
            continue
        if out:
            out[-1][1] = (out[-1][1] + " " + s).strip()
    return out


def add(proj, text, r=None):
    rv = load(proj)
    if not rv:
        rv = start(proj)
    new = parse_points(text)
    if not new:
        raise ValueError("時刻(0:07 など)の付いた行がありません")
    rd = _round(rv, r)
    for t, txt in new:
        rd["points"].append({"t": t, "text": txt, "done": False})
    rd["points"].sort(key=lambda p: p["t"])
    _write(proj, rv)
    return len(new)


def update(proj, i, r=None, **kw):
    rv = load(proj)
    rd = _round(rv, r)
    p = rd["points"][int(i)]
    for k in ("done", "text", "note"):
        if k in kw and kw[k] is not None:
            p[k] = kw[k]
    if kw.get("t") is not None:
        p["t"] = parse_time(kw["t"])
        p.pop("at_src", None)  # 時刻を直したら修正前の動画の時刻で当て直す
        rd["points"].sort(key=lambda p: p["t"])
    _write(proj, rv)


def delete(proj, i, r=None):
    rv = load(proj)
    del _round(rv, r)["points"][int(i)]
    _write(proj, rv)


def set_video(proj, video, r=None):
    rv = load(proj)
    _round(rv, r)["video"] = _rel(proj, video) if video else None
    _write(proj, rv)


# ---------------- 修正前と今の違い ----------------
def _sub(A, B):
    """区間の引き算 A - B(どちらも並べてまとめた区間の列)。"""
    out = []
    for a, b in A:
        cur = a
        for c, d in B:
            if d <= cur or c >= b:
                continue
            if c > cur:
                out.append((cur, c))
            cur = max(cur, d)
        if b - cur > 1e-3:
            out.append((cur, b))
    return [x for x in out if x[1] - x[0] > 1e-3]


def _label(kind, it):
    if kind == "sfx" or (kind == "overlays" and it.get("file")):
        return Path(str(it.get("file", ""))).name
    if kind == "overlays":
        return it.get("preset") or it.get("kind") or "差し込み"
    return str(it.get("text", "")).replace("\n", " ")[:40]


def _canon(it):
    return json.dumps(it, ensure_ascii=False, sort_keys=True)


def _t(it):
    try:
        return parse_time(it.get("gap_at")) if it.get("gap_at") is not None else parse_time(it.get("start", 0))
    except Exception:
        return 0.0


def changes(base, cur, tm_base, tm_cur):
    """修正前 → 今 の違い [{"src","what","label"}](元素材の時刻順)。"""
    out = []
    for a, b in _sub(tm_cur.cuts, tm_base.cuts):
        out.append({"src": a, "what": "カット", "label": f"{b - a:.2f}秒 削った"})
    for a, b in _sub(tm_base.cuts, tm_cur.cuts):
        out.append({"src": a, "what": "カット", "label": f"{b - a:.2f}秒 戻した"})
    gb, gc = dict(tm_base.gaps), dict(tm_cur.gaps)
    for at in sorted(set(gb) | set(gc)):
        if abs(gb.get(at, 0) - gc.get(at, 0)) > 1e-3:
            out.append({"src": at, "what": "空白", "label": f"{gb.get(at, 0):.2f} → {gc.get(at, 0):.2f}秒"})
    for kind, name in LISTS:
        B, C = base.get(kind) or [], cur.get(kind) or []
        bk, ck = {}, {}
        for it in B:
            bk.setdefault(_canon(it), []).append(it)
        for it in C:
            ck.setdefault(_canon(it), []).append(it)
        gone = [it for k, v in bk.items() for it in v[len(ck.get(k, [])):]]
        new = [it for k, v in ck.items() for it in v[len(bk.get(k, [])):]]
        for it in new:  # 同じ物(ファイル・文字)が近くで消えていれば「変更」
            m = next((g for g in gone if _label(kind, g) == _label(kind, it) and abs(_t(g) - _t(it)) < 30), None)
            if m:
                gone.remove(m)
                out.append({"src": _t(it), "what": name, "label": "変更 " + _label(kind, it)})
            else:
                out.append({"src": _t(it), "what": name, "label": "追加 " + _label(kind, it)})
        for it in gone:
            out.append({"src": _t(it), "what": name, "label": "削除 " + _label(kind, it)})
    skip = {"cuts", "gaps", "splits", *[k for k, _ in LISTS]}
    other = sorted(k for k in set(base) | set(cur) if k not in skip and _canon(base.get(k)) != _canon(cur.get(k)))
    if other:
        out.append({"src": None, "what": "設定", "label": "変更: " + ", ".join(other)})
    out.sort(key=lambda x: (x["src"] is None, x["src"] or 0))
    return out


def state(proj, r=None):
    rv = load(proj)
    if not rv:
        v = default_video(proj)
        return {"exists": False, "video_guess": _rel(proj, v) if v else None}
    rs = rounds(rv)
    r = len(rs) - 1 if r is None else max(0, min(len(rs) - 1, int(r)))
    rd = rs[r]
    base, cur = rd["base"], proj.data
    tb, tc = timemap(proj, base), timemap(proj, cur)
    pb, pc = pieces(tb), pieces(tc)
    pts = []
    for i, p in enumerate(rd["points"]):
        # at_src: 修正前の動画に無い所(差し替えた音声など)へ当てる点は、今の素材の秒で直接指す
        s, g = (float(p["at_src"]), None) if p.get("at_src") is not None else src_of(pb, float(p["t"]))
        pts.append({**p, "i": i, "src": round(s, 4), "gap": g})
    pv = False  # 修正前の動画の軽い版(720p・キーフレーム0.5秒おき)ができているか。/media?pv=a で返る
    if rd.get("video"):
        from . import proxy
        try:
            pv = proxy.video_proxy_path(proj, proj.resolve(rd["video"]), True).exists()
        except OSError:
            pass
    return {"exists": True, "round": r, "rounds": [{"i": i, "created": x.get("created"), "n": len(x.get("points") or []),
                                                    "done": sum(1 for q in x.get("points") or [] if q.get("done"))} for i, x in enumerate(rs)],
            "created": rd.get("created"), "video": rd.get("video"), "video_pv": pv,
            "base": {"pieces": pb, "total": tb.total}, "cur": {"pieces": pc, "total": tc.total},
            "points": pts, "changes": changes(base, cur, tb, tc)}


def _fmt_vr(t):
    """vreview が読む時刻: 分:秒 か 分:秒.百分の1秒(分は3桁まで。時は使わない)。"""
    c = int(round(max(0.0, float(t)) * 100))
    m, s = divmod(c, 6000)
    return f"{m}:{s // 100:02d}" + (f".{s % 100:02d}" if s % 100 else "")


def export_text(proj, undone=False, r=None):
    """修正点を vreview の管理ページに貼れる文章で出す(1行1点「時刻 内容」)。時刻は今の動画の時刻(パネルの今の時刻と同じ)。
    カットで消えた点はそのカットの継ぎ目の時刻に「[カット済] 」を付けて出す。undone=True で済みの点を除く。読むだけ。"""
    st = state(proj, r)
    if not st["exists"]:
        raise ValueError(f"修正点の控え({path_of(proj).name})がありません。先に vedit review … start / add で修正点を入れてください")
    if not st["points"]:
        raise ValueError("修正点が0件です")
    pc = st["cur"]["pieces"]
    rows = []
    for p in st["points"]:
        if undone and p.get("done"):
            continue
        s = p["src"]
        # パネル(review.js outOf)と同じ: 今の並びにあればその時刻、無ければ(カットで消えた)次に残る所の頭 = 継ぎ目
        hit = next((q for q in pc if "gap" not in q and q["s"] - 1e-6 <= s < q["e"]), None)
        if hit:
            o, cut = hit["o"] + s - hit["s"], False
        else:
            n = next((q for q in pc if "gap" not in q and q["s"] > s), None)
            o, cut = (n["o"] if n else st["cur"]["total"]), True
        text = " ".join(x.strip() for x in str(p.get("text") or "").splitlines() if x.strip()) or "(内容なし)"
        rows.append((round(o, 2), p["i"], ("[カット済] " if cut else "") + text))
    if not rows:
        raise ValueError("未済の修正点がありません")
    rows.sort()
    return "".join(f"{_fmt_vr(o)} {t}\n" for o, _, t in rows)
