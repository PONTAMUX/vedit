"""プリセット(vedit/presets/*.json)と効果音の音量そろえ("gain_db": "match:ぽペン")。
要素(overlays / telops)に "preset": "impact" と書くと、書き出し・プレビュー・vedit frame のときだけ複数の要素・効果音に展開する。
JSON にはプリセット名のまま残す(vedit expand で展開後の JSON を見られる)。

プリセットの書き方(presets/名前.json):
  {"doc": "説明",
   "params": {"text": null, "se": "...", ...},     # 既定値。要素に書いた同じ名前のキーで上書き。start / end / gap_at / len / n は自動
   "styles": {"impact": {...}},                    # プロジェクトに同じ名前のテロップの型が無ければ足す
   "items": [{"to": "overlays|telops|sfx|v1_fx|transitions", "if": "param" または "!param", "item": {...}}]}
  item の中の値:
    "$名前"          → params の値(null ならそのキーを書かない)
    "$=式"           → 数値の式(params の数値を名前で使える。例 "$=min(end, start + 1.6)")
    {"$pick": [...]} → n 番目(同じプリセットの何回目か)を順番に使う(効果音を回すなど)"""
import json
import os
import re
import subprocess
from pathlib import Path

from . import ff
from .timeutil import parse_time

PRESET_DIR = Path(__file__).parent / "presets"
TARGETS = ("overlays", "telops", "sfx", "v1_fx", "transitions")
AUTO = ("start", "end", "gap_at", "len", "n", "gap_len")  # 要素から自動で入る params
MATCH_BASE_DB = 1.0  # match: の基準の音を鳴らす大きさ(ぽペン +1dB)
MATCH_RANGE = (-12.0, 6.0)
# 効果音の名前(拡張子なし)を探す場所。環境変数 VEDIT_SE_DIRS(; 区切り)で足せる
SE_DIRS = [p for p in os.environ.get("VEDIT_SE_DIRS", "").split(";") if p] + ["se", "se"]
SE_EXT = (".mp3", ".wav", ".m4a", ".ogg", ".flac")

_PRESETS = None


def load():
    """名前 → プリセットの中身"""
    global _PRESETS
    if _PRESETS is None:
        _PRESETS = {p.stem: json.loads(p.read_text(encoding="utf-8")) for p in sorted(PRESET_DIR.glob("*.json"))}
    return _PRESETS


def catalog():
    """vedit fx list の presets"""
    return {n: {"doc": p.get("doc", ""), "params": p.get("params", {}),
                "makes": sorted({x["to"] for x in p.get("items", [])})} for n, p in load().items()}


# ---------------------------------------------------------------- 効果音の音量そろえ
def find_sound(name, proj=None):
    """"ぽペン" / "D:/…/ぽペン.mp3" / プロジェクトからの相対パス → Path(無ければ None)"""
    cands = []
    if proj is not None:
        cands.append(proj.resolve(name))
    cands.append(Path(name))
    for d in SE_DIRS:
        cands += [Path(d) / name] + [Path(d) / (name + e) for e in SE_EXT]
    for c in cands:
        if c.is_file():
            return c
    return None


_PEAKS = {}


def _peak_cache_file():
    return Path(os.environ.get("LOCALAPPDATA") or Path.home()) / "vedit" / "peaks.json"


def peak_db(f):
    """音のピーク(True Peak, dBFS)。ファイルの更新時刻つきでキャッシュ"""
    f = Path(f)
    st = f.stat()
    key = f"{f.resolve()}|{st.st_size}|{st.st_mtime_ns}"
    if not _PEAKS:
        try:
            _PEAKS.update(json.loads(_peak_cache_file().read_text(encoding="utf-8")))
        except (OSError, ValueError):
            pass
    if key not in _PEAKS:
        r = subprocess.run([ff.find("ffmpeg"), "-hide_banner", "-nostats", "-i", str(f), "-af", "apad=pad_dur=1,ebur128=peak=true",
                            "-f", "null", "-"], capture_output=True, text=True, encoding="utf-8", errors="replace", creationflags=ff.NOWIN).stderr
        m = re.findall(r"Peak:\s+(-?[\d.]+|-inf) dBFS", r)
        if not m:
            raise ValueError(f"音のピークが測れません: {f}")
        _PEAKS[key] = -120.0 if m[-1] == "-inf" else float(m[-1])
        try:
            p = _peak_cache_file()
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(json.dumps(_PEAKS, ensure_ascii=False), encoding="utf-8")
        except OSError:
            pass
    return _PEAKS[key]


_MATCH = re.compile(r"^match:(.+?)([+-]\d+(?:\.\d+)?)?$")


def parse_match(g):
    """"match:ぽペン-3" → ("ぽペン", -3.0)。match でなければ None"""
    m = _MATCH.match(g.strip()) if isinstance(g, str) else None
    return (m.group(1).strip(), float(m.group(2) or 0)) if m else None


def match_gain(g, file, proj=None):
    """gain_db の値 → dB。"match:基準[±n]" は基準の音(+1dB で鳴らす)とピークをそろえた大きさ + n(-12〜+6 に収める)"""
    if not isinstance(g, str):
        return float(g or 0)
    pm = parse_match(g)
    if not pm:
        return float(g)
    ref = find_sound(pm[0], proj)
    if ref is None:
        raise ValueError(f"gain_db {g!r}: 基準の音 {pm[0]!r} が見つかりません")
    f = proj.resolve(file) if proj is not None else Path(file)
    v = MATCH_BASE_DB + peak_db(ref) - peak_db(f)
    return round(max(MATCH_RANGE[0], min(MATCH_RANGE[1], v)) + pm[1], 1)


# ---------------------------------------------------------------- 展開
def _num(P):
    out = {}
    for k, v in P.items():
        if isinstance(v, bool):
            out[k] = float(v)
        elif isinstance(v, (int, float)):
            out[k] = float(v)
    return out


def _sub(v, P, n):
    from . import expr
    if isinstance(v, str):
        if v.startswith("$="):
            return round(expr.evaluate_vars(v[2:], **_num(P)), 4)
        if v.startswith("$") and v[1:] in P:
            return P[v[1:]]
        return v
    if isinstance(v, dict):
        if "$pick" in v:
            lst = v["$pick"]
            return _sub(lst[n % len(lst)], P, n) if lst else None
        out = {}
        for k, x in v.items():
            y = _sub(x, P, n)
            if y is not None:
                out[k] = y
        return out
    if isinstance(v, list):
        return [_sub(x, P, n) for x in v]
    return v


def _gap_len(data, at):
    for g in data.get("gaps") or []:
        if abs(parse_time(g[0]) - at) < 1e-3:
            return float(g[1])
    return None


def instance(data, el, n):
    """プリセットの要素1つ → {"overlays": [...], ...}(展開した要素には "_preset": 名前 が付く)"""
    name = el["preset"]
    spec = load().get(name)
    if spec is None:
        raise ValueError(f"{name!r} というプリセットはありません(使えるもの: {', '.join(load())})")
    P = {k: _sub(v, {}, n) if isinstance(v, dict) and "$pick" in v else v for k, v in spec.get("params", {}).items()}
    P.update({k: v for k, v in el.items() if k != "preset"})
    P["n"] = n
    if el.get("gap_at") is not None:  # 空白の中: start/end は空白の頭からの秒(省略で空白の全部)
        gl = _gap_len(data, parse_time(el["gap_at"]))
        P["gap_len"] = gl
        if P.get("start") is None:
            P["start"] = 0.0
        if P.get("end") is None:
            P["end"] = gl
    for k in ("start", "end"):
        if P.get(k) is not None:
            P[k] = parse_time(P[k])
    if P.get("start") is not None and P.get("end") is not None:
        P["len"] = P["end"] - P["start"]
    out = {k: [] for k in TARGETS}
    for x in spec.get("items", []):
        cond = x.get("if")
        if cond:
            neg = cond.startswith("!")
            val = P.get(cond.lstrip("!"))
            if bool(val) == neg:
                continue
        it = _sub(x["item"], P, n)
        it["_preset"] = name
        out[x["to"]].append(it)
    return out, spec.get("styles") or {}


def has_work(data):
    """展開するものがあるか(プリセット・match の音量)"""
    if any(isinstance(it, dict) and it.get("preset") for k in ("overlays", "telops") for it in data.get(k) or []):
        return True
    if any(isinstance(s.get("gain_db"), str) for s in data.get("sfx") or []):
        return True
    return any(isinstance(((t.get("se") or {}) if isinstance(t, dict) else {}).get("gain_db"), str) for t in data.get("transitions") or [])


def expand_data(data, proj=None, gains=True):
    """プリセットを展開し、match の音量を数値にした data(浅いコピー)。何も無ければ data をそのまま返す。
    プリセットでない要素は同じ dict のまま(GUI のプレビューが番号を対応させるため)"""
    if not has_work(data):
        return data
    d = dict(data)
    lists = {k: [] for k in TARGETS}
    styles = {}
    count = {}
    for k in ("overlays", "telops"):
        for el in data.get(k) or []:
            if isinstance(el, dict) and el.get("preset"):
                n = count.get(el["preset"], 0)
                count[el["preset"]] = n + 1
                o, st = instance(data, el, n)
                for t, lst in o.items():
                    lists[t] += lst
                for sn, sv in st.items():
                    styles.setdefault(sn, sv)
            else:
                lists[k].append(el)
    for k in TARGETS:
        own = [] if k in ("overlays", "telops") else list(data.get(k) or [])
        d[k] = own + lists[k] if k not in ("overlays", "telops") else lists[k]
    if styles:
        d["styles"] = {**styles, **(data.get("styles") or {})}
    if gains:
        sf = []
        for s in d.get("sfx") or []:
            if isinstance(s.get("gain_db"), str):
                s = {**s, "gain_db": match_gain(s["gain_db"], s["file"], proj), "_match": s["gain_db"]}
            sf.append(s)
        d["sfx"] = sf
        tr = []
        for t in d.get("transitions") or []:
            se = t.get("se") if isinstance(t, dict) else None
            if se and isinstance(se.get("gain_db"), str):
                t = {**t, "se": {**se, "gain_db": match_gain(se["gain_db"], se["file"], proj)}}
            tr.append(t)
        d["transitions"] = tr
    return d


def validate(data, proj=None):
    """プリセットの要素と match の音量の誤り"""
    errs = []
    ps = load()
    for k in ("overlays", "telops"):
        for i, el in enumerate(data.get(k) or []):
            if not isinstance(el, dict) or not el.get("preset"):
                continue
            w = f"{k}[{i}]"
            spec = ps.get(el["preset"])
            if spec is None:
                errs.append(f"{w}: {el['preset']!r} というプリセットはありません(使えるもの: {', '.join(ps)})")
                continue
            allowed = set(spec.get("params", {})) | set(AUTO) | {"preset"}
            for key in el:
                if key not in allowed:
                    errs.append(f"{w}: プリセット {el['preset']} に {key!r} は使えません(使えるもの: {', '.join(sorted(set(spec.get('params', {}))))})")
            if el.get("start") is None or el.get("end") is None:  # GUI のタイムラインに置くため、空白の中(gap_at)でも書く
                errs.append(f"{w}: プリセットには start と end が必要です(gap_at の時は空白の頭からの秒。例: start 0, end 空白の長さ)")
            for need in spec.get("required", []):
                if isinstance(need, list):
                    if not any(el.get(x) for x in need):
                        errs.append(f"{w}: プリセット {el['preset']} には {' か '.join(need)} が必要です")
                elif el.get(need) in (None, ""):
                    errs.append(f"{w}: プリセット {el['preset']} には {need} が必要です")
            if el.get("gap_at") is not None and _gap_len(data, parse_time(el["gap_at"])) is None:
                errs.append(f"{w}: gap_at {el['gap_at']} の空白(gaps)がありません")
            try:
                instance(data, el, 0)
            except Exception as e:  # noqa: BLE001
                errs.append(f"{w}: プリセットを展開できません({e})")

    def chk_gain(w, g, file):
        if not isinstance(g, str):
            return
        pm = parse_match(g)
        if not pm:
            try:
                float(g)
            except ValueError:
                errs.append(f"{w}.gain_db: 数値か \"match:基準の音[±dB]\" にしてください({g!r})")
            return
        if find_sound(pm[0], proj) is None:
            errs.append(f"{w}.gain_db: 基準の音 {pm[0]!r} が見つかりません(探す場所: {', '.join(SE_DIRS)})")

    for i, s in enumerate(data.get("sfx") or []):
        chk_gain(f"sfx[{i}]", s.get("gain_db"), s.get("file"))
    for i, t in enumerate(data.get("transitions") or []):
        se = t.get("se") or {}
        chk_gain(f"transitions[{i}].se", se.get("gain_db"), se.get("file"))
    return errs
