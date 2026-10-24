"""プロジェクト → ffmpeg 1コマンドで書き出し(カット・音声・映像差し込み・テロップ・トピックバー・BGM)。"""
import json
import math
import os
import subprocess
from pathlib import Path

from . import ass, audio, ff, fx as vfx
from .timeutil import TimeMap, parse_time

POS = {
    "center": "(W-w)/2:(H-h)/2",
    "top-left": "{m}:{m}", "top-right": "W-w-{m}:{m}",
    "bottom-left": "{m}:H-h-{m}", "bottom-right": "W-w-{m}:H-h-{m}",
}


class Plan:
    """読み込み済みの情報と、カット後の時刻に換算した各要素。"""

    def __init__(self, proj):
        from . import presets
        d = presets.expand_data(proj.data, proj)  # プリセットの展開・match の音量(無ければ proj.data のまま)
        if d is not proj.data:
            import copy
            proj = copy.copy(proj)
            proj.data = d
        self.proj = proj
        self.src = proj.resolve(d["source"])
        self.info = ff.probe(self.src)
        self.W, self.H = self.info["width"], self.info["height"]
        self.fps = d["output"].get("fps") or round(self.info["fps"], 3)
        self.tm = TimeMap(self.info["duration"], [(parse_time(a), parse_time(b)) for a, b in d["cuts"]],
                          [(parse_time(a), float(b)) for a, b in d.get("gaps") or []])
        self.total = self.tm.total
        self.keeps = self.tm.keeps
        self.pieces = self.tm.pieces()  # ("src",a,b) / ("gap",長さ) を出力順に
        # リンク解除した編集(出力上の時刻に換算): 映像だけ黒 / 音声だけ無音 / 音声だけ移動
        rngs = lambda lst: [r for r in (self.tm.out_range(parse_time(a), parse_time(b)) for a, b in lst) if r]
        self.black = rngs(d.get("vmute") or [])
        self.silent = rngs(d.get("amute") or [])
        self.slips = []
        for sl in d.get("aslip") or []:
            a, b, dd = parse_time(sl["a"]), parse_time(sl["b"]), float(sl.get("d", 0))
            o = self.tm.out(a)
            self.silent.append((o, o + b - a))              # 元の場所は無音
            self.silent.append((o + dd, o + dd + b - a))    # 移動先は上書き(元の音を消す)
            self.slips.append((a, b, max(0.0, o + dd)))
        # トラックの表示/ミュート(GUI の目・M ボタン)。hide: v1/v2../tb/t、mute: a1/se/bgm
        self.hide, self.mute = set(d.get("hide") or []), set(d.get("mute") or [])
        if "v1" in self.hide:
            self.black = [(0.0, self.total)]
        if "a1" in self.mute:
            self.silent, self.slips = [(0.0, self.total)], []

    def overlays(self):
        """出す差し込み: 非表示のトラックを除き、トラック番号の小さい順(大きいほど上に重なる)。同じトラック内はリストの順。"""
        tr = lambda it: max(2, round(float(it.get("track") or 2)))
        return sorted((it for it in self.proj.data["overlays"] if f"v{tr(it)}" not in self.hide), key=tr)

    def sfx(self):
        """効果音を出力上の時刻に換算 → [(a, b, it)]。時刻の基準とカットの扱いは overlays と同じ(conv)。
        it は start/end/in を秒にそろえたコピー。end 省略時は「in から音の終わりまで」。"""
        items = []
        for it in self.proj.data.get("sfx") or []:
            if ("bgm" if it.get("kind") == "bgm" else "se") in self.mute:  # kind=bgm は BGM トラック
                continue
            f = self.proj.resolve(it["file"])
            a, skip = parse_time(it["start"]), parse_time(it.get("in", 0))
            b = parse_time(it["end"]) if it.get("end") is not None else a + max(0.0, ff.probe(f)["duration"] - skip)
            items.append({**it, "start": a, "end": b, "in": skip})
        res = self.conv(items)
        if "se" not in self.mute:
            res += vfx.extra_sfx(self)  # トランジションの効果音(出力上の時刻)
        return res

    def ovs_out(self, when=None):
        """出す差し込みを出力上の時刻で [(a,b,it)]。エフェクト・生成要素・トランジションは焼いたキャッシュに置き換え済み。
        when=(lo,hi) ならその範囲に出るものだけ焼く。"""
        return vfx.expand(self, self.conv(self.overlays()), when=when)

    def conv(self, items):
        out = []
        for it in items:
            a, b = parse_time(it["start"]), parse_time(it["end"])
            if it.get("gap_at") is not None:  # 空白の中に置いたもの(ドドンの効果音・章見出し): start/end は空白の頭からの秒
                g = self.tm.gap_at(parse_time(it["gap_at"]))
                r = (g[0] + max(0.0, a), g[0] + min(b, g[1])) if g else None
            else:
                r = self.tm.out_range(a, b) if self.proj.data["timebase"] == "source" else (a, min(b, self.total))
            if r and r[1] > r[0]:
                out.append((r[0], r[1], it))
        return out


def _overlay_pos(it, plan):
    k = plan.H / 1080.0
    pos = it.get("pos", "center" if it.get("scale") is None else "top-right")
    if isinstance(pos, (list, tuple)):
        return f"{round(pos[0] * k)}:{round(pos[1] * k)}"
    if pos == "full":
        return "0:0"
    return POS[pos].format(m=round(40 * k))


def _scale_filter(it, plan):
    pos = it.get("pos", "full" if it.get("scale") is None else "top-right")
    if pos == "full":
        W, H = plan.W, plan.H
        if it.get("fit") == "cover":
            return f"scale={W}:{H}:force_original_aspect_ratio=increase,crop={W}:{H}"
        return f"scale={W}:{H}:force_original_aspect_ratio=decrease,pad={W}:{H}:(ow-iw)/2:(oh-ih)/2:color=black"
    w = int(plan.W * float(it.get("scale", 0.3)) / 2) * 2
    return f"scale={w}:-2"


def _gpu_video_ok(it, plan, f):
    """差し込み動画を GPU のまま縮小・合成できるか(全画面で縦横比が同じ・透明度のフェードなし)。"""
    pos = it.get("pos", "full" if it.get("scale") is None else "top-right")
    if pos != "full" or "fade" in (it.get("in_anim"), it.get("out_anim")):
        return False
    try:
        i = ff.probe(f)
        return abs(i["width"] / i["height"] - plan.W / plan.H) < 0.01
    except Exception:
        return False


def _engine_layer(plan, a, b, it, win):
    """差し込み1つを自前エンジン(engine.py)への指示にする。時刻・位置・アニメの計算は build の ffmpeg の式と同じ。
    デコードと縮小は ffmpeg(同じ scale フィルタ)に任せ、透明度つきの yuva420p の生の絵をもらう。
    動画は fps=..:round=up で出力のコマの格子にそろえる(overlay が「その時刻までで最後のコマ」を使うのと同じ)"""
    proj, F = plan.proj, plan.fps
    f = proj.resolve(it["file"])
    is_img = f.suffix.lower() in IMG_EXT
    pos = it.get("pos", "full" if it.get("scale") is None else "top-right")
    sf = _scale_filter(it, plan)
    if is_img:
        from PIL import Image
        f = _small_image(proj, plan, it, f)
        with Image.open(f) as im:
            iw, ih = im.size
    else:
        i = ff.probe(f)
        iw, ih = i["width"], i["height"]
    if pos == "full":
        w, h = plan.W, plan.H
    else:
        w = int(plan.W * float(it.get("scale", 0.3)) / 2) * 2
        h = max(2, round(w * ih / (iw * 2)) * 2)  # scale=w:-2 と同じ
    k = plan.H / 1080.0
    p0 = it.get("pos", "center" if it.get("scale") is None else "top-right")
    m = round(40 * k)
    W, H = plan.W, plan.H
    if isinstance(p0, (list, tuple)):
        x0, y0 = round(p0[0] * k), round(p0[1] * k)
    else:
        x0, y0 = {"full": (0, 0), "center": ((W - w) / 2, (H - h) / 2), "top-left": (m, m), "top-right": (W - w - m, m),
                  "bottom-left": (m, H - h - m), "bottom-right": (W - w - m, H - h - m)}[p0]
    dur = min(float(it.get("anim_dur", 0.3)), (b - a) / 2)
    a4, b4 = round(a, 4), round(b, 4)
    # 出すコマ(出力全体のコマ番号): between(t,a,b) の範囲で、この区間に入るもの
    lo, hi = round(win[0] * F), round(win[1] * F)
    m0 = max(lo, math.ceil(a4 * F - 1e-6))
    m1 = min(hi - 1, math.floor(b4 * F + 1e-6))
    L = {"file": str(f), "img": is_img, "w": w, "h": h, "x": x0, "y": y0, "a": a4, "b": b4, "dur": round(dur, 3),
         "bd": round(b - dur, 4), "in": it.get("in_anim", "none"), "out": it.get("out_anim", "none"), "m0": m0, "m1": m1}
    # 透明のない動画は yuva420p にしても透明度は全部 255 で、重ねると下が丸ごと隠れる。yuv420p でもらってそのまま写す
    pf = "" if is_img else (_vinfo(f).get("pix_fmt") or "a")
    L["alpha"] = is_img or any(x in pf for x in ("a", "pal"))  # yuva*/rgba/bgra/argb/gbrap/ya8/pal8 など(不明も透明ありとみなす)
    fmt = f"{sf},format={'yuva420p' if L['alpha'] else 'yuv420p'}"
    if is_img:
        L["dec"] = ["-i", str(f), "-vf", fmt, "-frames:v", "1"]
    else:
        inn = parse_time(it.get("in", 0))
        skip = max(0.0, win[0] - a)
        if skip:  # build と同じ読み方(1コマ手前からシーク、時刻はそのまま)
            ss = max(0.0, inn + skip - 1 / F)
            src = ["-ss", f"{ss:.4f}", "-i", str(f)]
            pre = f"setpts=PTS+{a + ss - inn:.4f}/TB,tpad=stop_mode=clone:stop_duration=0.5"
        else:
            src = ["-i", str(f)]
            pre = f"trim=start={inn},setpts=PTS-STARTPTS+{a:.4f}/TB,tpad=stop_mode=clone:stop_duration=0.5"
        vd = ff.probe(f).get("duration")
        L["vend"] = round(a + float(vd) - inn, 4) if vd else None  # 素材が尽きる出力の時刻
        L["dec"] = [*src, "-vf", f"{pre},fps=fps={F}:round=up:start_time={(m0 - 0.5) / F:.6f},{fmt}",
                    "-frames:v", str(max(1, m1 - m0 + 1))]
    return L


def engine_on(proj):
    """書き出しエンジンに自前エンジン(engine.py)を使うか(書き出し設定の「書き出しエンジン」。既定は ffmpeg)。"""
    return proj.data["output"].get("engine") == "vedit"


_NVDEC = {}
VRAM_PER_CHUNK = 1000  # GPU 合成の1区間(ffmpeg 1本)に見込む VRAM(MiB)。実測 0.7〜0.8GB
NVDEC_MAX = 3  # GPU 合成で1区間あたり NVDEC で読む差し込み動画の上限(1本ごとに VRAM を数百MB使う。並列の区間ぶん掛け算になる)


def _vinfo(f):
    """動画の形式と色の情報(ffprobe。更新時刻ごとに覚えておく)。"""
    try:
        key = (str(f), os.stat(f).st_mtime_ns)
    except OSError:
        return {}
    if key not in _NVDEC:
        try:
            out = subprocess.run([ff.find("ffprobe"), "-v", "error", "-select_streams", "v:0", "-show_entries",
                                  "stream=codec_name,pix_fmt,width,color_range,color_space", "-of", "json", str(f)],
                                 capture_output=True, text=True, encoding="utf-8", creationflags=ff.NOWIN).stdout
            _NVDEC[key] = (json.loads(out).get("streams") or [{}])[0]
        except Exception:
            _NVDEC[key] = {}
    return _NVDEC[key]


def _nvdec_ok(f):
    """NVDEC でそのまま読める動画か(8bit 4:2:0 の H.264/HEVC/VP9/AV1。透明つきの .mov などは CPU で読む)。"""
    v = _vinfo(f)
    return (v.get("codec_name") in ("h264", "hevc", "vp9", "av1") and v.get("pix_fmt") in ("yuv420p", "yuvj420p", "nv12")
            and int(v.get("width") or 0) <= 4096)


def _known(x):
    return None if x in (None, "", "unknown", "unspecified", "reserved") else x


def _color_fix(f, base):
    """GPU で重ねる差し込み動画の色情報を下地(base=(range, colorspace))にそろえるフィルタ。
    CPU の overlay は色情報の違う絵を自動で変換して重ねるが、overlay_cuda は変換できずに失敗する。
    書いていないだけなら下地と同じと書く("")。違う値が書いてある(本当に変換が要る)なら None(その区間は CPU で重ねる)。
    RGB の素材(qtrle・PNG 連番など)は YUV にするときに下地に合わせて変換されるので何もしない"""
    v = _vinfo(f)
    if not str(v.get("pix_fmt") or "").startswith(("yuv", "nv")):
        return ""
    rg, cs = _known(v.get("color_range")), _known(v.get("color_space"))
    if (rg or base[0]) != base[0] or (cs or base[1]) != base[1]:
        return None
    return f",setparams=range={base[0]}:colorspace={base[1]}" if not (rg and cs) else ""


def gpu_accel(proj):
    """書き出しの重ね合わせを GPU(CUDA)でするか。書き出し設定の「GPU 合成」(既定はオン)と、GPU で実際に動くか。"""
    return proj.data["output"].get("gpu_accel", True) is not False and ff.has_cuda()


def _small_image(proj, plan, it, f):
    """画像を出す大きさまで縮めた PNG をキャッシュに作って返す(大きい画像のままだと ffmpeg が全部デコードして抱え、メモリを食う)。"""
    import hashlib
    from PIL import Image
    pos = it.get("pos", "full" if it.get("scale") is None else "top-right")
    try:
        with Image.open(f) as im:
            iw, ih = im.size
            if pos == "full":
                k = (max if it.get("fit") == "cover" else min)(plan.W / iw, plan.H / ih)
            else:
                k = int(plan.W * float(it.get("scale", 0.3)) / 2) * 2 / iw
            if k >= 0.95:  # 縮めても小さくならない
                return f
            st = f.stat()
            key = hashlib.sha1(f"{f}|{st.st_mtime_ns}|{st.st_size}|{k:.5f}".encode()).hexdigest()[:16]
            out = proj.cache_dir / "rimg" / f"{key}.png"
            if not out.exists():
                out.parent.mkdir(parents=True, exist_ok=True)
                im2 = im.convert("RGBA") if im.mode not in ("RGB", "RGBA") else im
                im2.resize((max(2, round(iw * k)), max(2, round(ih * k))), Image.LANCZOS).save(out)
            return out
    except Exception:
        return f


IMG_EXT = (".png", ".jpg", ".jpeg", ".webp", ".bmp")


def _image_tracks(ovs, plan):
    """全画面・アニメなし・画面と同じ大きさの画像で、時間が重ならないものを1本の連番トラックにまとめる(テロップ画像など)。
    数百枚を別々の入力にすると ffmpeg が全部を同時に開いて重くなるため。重なり順は変えない:
    トラックは最初の要素の位置で合成し、その間に入る他の差し込みと時間が重なる要素は加えない。
    戻り値: (トラックのリスト, 残す差し込み(各トラックは最初の要素だけ残る))"""
    try:
        from PIL import Image
    except ImportError:
        return [], ovs
    sizes = {}

    def simple(it):
        f = plan.proj.resolve(it["file"])
        if f.suffix.lower() not in IMG_EXT or it.get("pos", "center") != "full" or it.get("scale") is not None:
            return False
        if it.get("in_anim", "none") != "none" or it.get("out_anim", "none") != "none":
            return False
        if f not in sizes:
            try:
                with Image.open(f) as im:
                    sizes[f] = im.size
            except OSError:
                sizes[f] = None
        return sizes[f] == (plan.W, plan.H)

    tracks = []  # [最初の要素の番号, [(a,b,it),...]]
    for i, (a, b, it) in enumerate(ovs):
        if not simple(it):
            continue
        for tr in tracks:
            f0, mem = tr
            if mem[-1][1] > a + 1e-6:
                continue
            ids = {id(m[2]) for m in mem}
            if any(ovs[j][0] < b and a < ovs[j][1] and id(ovs[j][2]) not in ids for j in range(f0 + 1, i)):
                continue
            mem.append((a, b, it))
            break
        else:
            tracks.append([i, [(a, b, it)]])
    tracks = [mem for _, mem in tracks if len(mem) >= 2]
    drop = {id(m[2]) for mem in tracks for m in mem[1:]}
    return tracks, [o for o in ovs if id(o[2]) not in drop]


def _video_tracks(ovs, plan):
    """続けて並ぶ動画の差し込み(全画面・アニメなし・音なし・同じトラック・時間が重ならない。ゲーム映像など)を1本にまとめる。
    1本ずつ別の入力にして setpts で先の時刻へ送ると、ffmpeg が全部を頭から同時にデコードし、まだ出番の来ない先の映像が
    メモリに溜まり続ける(56本で 22GB 使って止まった)。透明のつなぎと concat して、頭から順に1本で流す。
    戻り値: (トラックのリスト, 残す差し込み(各トラックは最初の要素だけ残る))"""
    tr = lambda it: it.get("_z") or max(2, round(float(it.get("track") or 2)))

    def simple(it):
        return (plan.proj.resolve(it["file"]).suffix.lower() not in IMG_EXT and it.get("pos", "full") == "full"
                and it.get("scale") is None and not it.get("audio")
                and it.get("in_anim", "none") == "none" and it.get("out_anim", "none") == "none")
    tracks, run = [], []
    for o in ovs + [None]:
        if o and simple(o[2]) and (not run or (tr(o[2]) == tr(run[-1][2]) and o[0] >= run[-1][1] - 1e-6)):
            run.append(o)
            continue
        if len(run) >= 2:
            tracks.append(run)
        run = [o] if o and simple(o[2]) else []
    drop = {id(m[2]) for mem in tracks for m in mem[1:]}
    return tracks, [o for o in ovs if id(o[2]) not in drop]


def video_track_graph(proj, plan, mem, inputs, g, nin, label):
    """_video_tracks の1本: 各動画を出る長さぴったりのフレーム数に切り、間を透明でつないで concat。戻り値: 次の入力番号"""
    F, segs, pos = plan.fps, [], 0
    for k, (a, b, it) in enumerate(mem):
        f0, f1 = int(round(a * F)), int(round(b * F))
        if f0 > pos:
            g.append(f"color=c=black@0.0:s={plan.W}x{plan.H}:r={F},format=yuva420p,trim=end_frame={f0 - pos},setsar=1[{label}z{k}]")
            segs.append(f"[{label}z{k}]")
        n = max(1, f1 - max(f0, pos))
        inputs += ["-i", proj.resolve(it["file"])]
        g.append(f"[{nin}:v]trim=start={parse_time(it.get('in', 0)):.4f},setpts=PTS-STARTPTS,fps={F},tpad=stop_mode=clone:stop=-1,"
                 f"trim=end_frame={n},setpts=PTS-STARTPTS,{_scale_filter(it, plan)},format=yuva420p,setsar=1[{label}v{k}]")
        segs.append(f"[{label}v{k}]")
        pos, nin = max(f0, pos) + n, nin + 1
    g.append(f"{''.join(segs)}concat=n={len(segs)}:v=1:a=0[{label}]")
    return nin


def cache_track(proj, plan, mem, j):
    """連番トラックの ffconcat(間は透明画像)を書いてパスを返す。"""
    from PIL import Image
    cache = proj.cache_dir
    blank = cache / f"blank_{plan.W}x{plan.H}.png"
    if not blank.exists():
        Image.new("RGBA", (plan.W, plan.H), (0, 0, 0, 0)).save(blank)
    q = lambda p: "file '" + str(Path(p).resolve()).replace("\\", "/").replace("'", "'\\''") + "'"
    lines, t = ["ffconcat version 1.0"], 0.0
    for a, b, it in mem:
        if a > t + 1e-4:
            lines += [q(blank), f"duration {a - t:.4f}"]
        lines += [q(proj.resolve(it["file"])), f"duration {b - a:.4f}"]
        t = b
    lines += [q(blank), "duration 1", q(blank)]  # 最後の duration が効くように同じ画像をもう1つ
    p = cache / f"track{j}.ffconcat"
    p.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return p.name


SR = 48000
AFMT = "aformat=sample_fmts=fltp:sample_rates=48000:channel_layouts=stereo"


def _sfx_graph(plan, inputs, g, nin):
    """効果音を、重ならないものどうしを1本にまとめた数本のトラック(無音+効果音の concat)にする。
    効果音ごとに adelay で全編ぶんの無音を作ると数百個で重くなるため。時刻はサンプル単位で合わせる。
    同じファイルは入力を1つにして asplit で配る。戻り値: (トラックのラベル, 次の入力番号)"""
    items = sorted(plan.sfx(), key=lambda x: x[0])
    if not items:
        return [], nin
    layers = []
    for a, b, it in items:
        for L in layers:
            if L[-1][1] <= a + 1e-6:
                L.append((a, b, it))
                break
        else:
            layers.append([(a, b, it)])
    uses = {}
    for L in layers:
        for _, _, it in L:
            uses.setdefault(str(plan.proj.resolve(it["file"])), []).append(id(it))
    lab = {}  # id(it) → 入力ラベル
    for f, ids in uses.items():
        inputs += ["-i", f]
        outs = [f"sx{nin}_{k}" for k in range(len(ids))]
        g.append(f"[{nin}:a]{AFMT}," + (f"asplit={len(ids)}" if len(ids) > 1 else "anull") + "".join(f"[{o}]" for o in outs))
        lab.update(zip(ids, outs))
        nin += 1
    labels = []
    for j, L in enumerate(layers):
        segs, pos = [], 0
        for k, (a, b, it) in enumerate(L):
            s0 = max(pos, int(round(a * SR)))
            s1 = max(s0 + 1, int(round(b * SR)))  # conv で 1ms 未満は除外済み
            if s0 > pos:
                g.append(f"anullsrc=r={SR}:cl=stereo,atrim=end_sample={s0 - pos},{AFMT}[sz{j}_{k}]")
                segs.append(f"[sz{j}_{k}]")
            g.append(f"[{lab[id(it)]}]atrim=start={it['in']:.4f},asetpts=PTS-STARTPTS,volume={float(it.get('gain_db', 0)):.2f}dB,"
                     f"apad,atrim=end_sample={s1 - s0}[se{j}_{k}]")
            segs.append(f"[se{j}_{k}]")
            pos = s1
        g.append(f"{''.join(segs)}concat=n={len(segs)}:v=0:a=1[sfx{j}]")
        labels.append(f"[sfx{j}]")
    return labels, nin


def _frame_piece(plan, at):
    """静止画用: 時刻 at(カット後)を含む区間だけを前後0.5秒に切り詰めて返す → (piece, 出力上の開始時刻, 入力シーク秒)。"""
    o = 0.0
    for pc in plan.pieces:
        L = pc[1] if pc[0] == "gap" else pc[2] - pc[1]
        if at < o + L or pc is plan.pieces[-1]:
            if pc[0] == "gap":
                return pc, o, 0.0
            s = pc[1] + max(0.0, at - o)
            a, b = max(pc[1], s - 0.5), min(pc[2], s + 0.5)
            return ("src", a, b), o + (a - pc[1]), a
        o += L
    return None


def _cut_wav(proj, P, fx):
    """カット後の音声を numpy で1本の wav にする(区間の頭と終わりに短いフェード、空白は無音)。キャッシュする。"""
    import hashlib
    import numpy as np
    import soundfile as sf
    wav = proj.cache_dir / "audio.wav"
    st = wav.stat()
    key = hashlib.sha1(json.dumps([str(st.st_mtime_ns), st.st_size, fx.get("audio_fade_ms"), P]).encode()).hexdigest()[:16]
    out = proj.cache_dir / f"cut_{key}.wav"
    if out.exists():
        return out
    for old in proj.cache_dir.glob("cut_*.wav"):
        old.unlink(missing_ok=True)
    with sf.SoundFile(str(wav)) as f:
        sr, ch = f.samplerate, f.channels
        total = 0.0
        for pc in P:
            total += pc[1] if pc[0] == "gap" else pc[2] - pc[1]
        buf = np.zeros((round(total * sr) + sr, ch), np.float32)
        t, n = 0.0, len(P)
        for i, pc in enumerate(P):
            o = round(t * sr)
            if pc[0] == "gap":
                t += pc[1]
                continue
            _, a, b = pc
            f.seek(round(a * sr))
            x = f.read(round(b * sr) - round(a * sr), dtype="float32", always_2d=True)
            L = len(x)
            fl = min(int(fx.get("audio_fade_ms", 8) / 1000 * sr), L // 3)
            if fl > 0 and n > 1:
                r = np.linspace(0, 1, fl, endpoint=False, dtype=np.float32)[:, None]
                if i > 0:
                    x[:fl] *= r
                if i < n - 1:
                    x[L - fl:] *= r[::-1]
            buf[o:o + L] = x[:len(buf) - o]
            t += b - a
        buf = buf[:round(total * sr)]
    tmp = out.with_suffix(".tmp.wav")
    sf.write(str(tmp), buf, sr, subtype="PCM_24")
    tmp.replace(out)
    return out


def _hide_pieces(plan, P, win, layers):
    """自前エンジン用: 全画面・透明なし・動きなしの差し込み動画に全部のコマが隠れる元動画の区間を、同じ長さの黒(gap)にする。
    隠れて見えないので絵は変わらず、元動画のデコード(ffmpeg 方式でも全部している)を省ける。
    区間の途中では切らない(コマ数の割り振りは区間の長さの積み上げで決まるので、丸ごと置き換えれば他のコマは1つもずれない)"""
    F, W, H = plan.fps, plan.W, plan.H
    hid = set()
    for L in layers:
        if L["img"] or L.get("alpha", True) or (L["x"], L["y"], L["w"], L["h"]) != (0, 0, W, H):
            continue
        lo, hi = L["m0"], L["m1"]
        if L["in"] != "none":
            lo = max(lo, math.ceil((L["a"] + L["dur"]) * F) + 1)
        if L["out"] != "none":
            hi = min(hi, math.floor(L["bd"] * F) - 1)
        if L.get("vend") is not None:  # 動画が尺より短いと途中で消える(下が見える)
            hi = min(hi, math.floor(L["vend"] * F) - 2)
        hid.update(range(lo, hi + 1))
    if not hid:
        return P
    m, o, out = round(win[0] * F), 0.0, []
    for pc in P:
        L = pc[1] if pc[0] == "gap" else pc[2] - pc[1]
        k0, k1 = math.ceil(o * F - 1e-6), math.ceil((o + L) * F - 1e-6)  # build と同じ割り振り
        o += L
        if pc[0] == "src" and k1 > k0 and all(m + k in hid for k in range(k0, k1)):
            pc = ("gap", L)
        out.append(pc)
    return out


def _win_pieces(plan, A, B):
    """出力の A〜B 秒にあたる区間 → (区間リスト, 入力シーク秒)。区間の元時刻はシークぶん引いてある。"""
    out, acc = [], 0.0
    for pc in plan.pieces:
        L = pc[1] if pc[0] == "gap" else pc[2] - pc[1]
        lo, hi = max(A, acc), min(B, acc + L)
        if hi - lo > 0.5 / plan.fps:  # 1フレームに満たない切れ端は捨てる(長さは -frames:v で合わせる)
            out.append(("gap", hi - lo) if pc[0] == "gap" else ("src", pc[1] + lo - acc, pc[1] + hi - acc))
        acc += L
    srcs = [x[1] for x in out if x[0] == "src"]
    seek = max(0.0, min(srcs) - 1.0) if srcs else 0.0
    return [x if x[0] == "gap" else (x[0], x[1] - seek, x[2] - seek) for x in out], seek


def build_audio(plan):
    """音声だけの (入力引数, filter_complex, 音声ラベル)。カット後の音声は _cut_wav で1本にしてある前提。"""
    proj, d = plan.proj, plan.proj.data
    wav = proj.cache_dir / "audio.wav"
    fx = {"audio_fade_ms": 8, "video": "none", "dur": 0.12, **(d.get("cut_fx") or {})}
    inputs = ["-i", _cut_wav(proj, plan.pieces, fx), "-i", wav]
    g = ["[0:a]aformat=sample_fmts=fltp:sample_rates=48000:channel_layouts=stereo[acat0]"]
    en = lambda rs: "+".join(f"between(t,{a:.4f},{b:.4f})" for a, b in rs)
    g.append(f"[acat0]volume=0:enable='{en(plan.silent)}'[acat]" if plan.silent else "[acat0]anull[acat]")
    nin, extra = 2, []
    for k, (a, b, o) in enumerate(plan.slips):
        ms, fd = int(round(o * 1000)), min(0.008, (b - a) / 3)
        g.append(f"[1:a]atrim=start={a:.4f}:end={b:.4f},asetpts=PTS-STARTPTS,afade=t=in:d={fd:.4f},afade=t=out:st={b - a - fd:.4f}:d={fd:.4f},"
                 f"adelay={ms}:all=1[sl{k}]")
        extra.append(f"[sl{k}]")
    for j, (a, b, it) in enumerate(plan.conv(plan.overlays())):  # 差し込み動画の音
        if not it.get("file"):
            continue
        f = proj.resolve(it["file"])
        if not it.get("audio") or f.suffix.lower() in IMG_EXT or not ff.probe(f)["has_audio"]:
            continue
        inputs += ["-i", f]
        ms = int(a * 1000)
        g.append(f"[{nin}:a]atrim=start={parse_time(it.get('in', 0))}:duration={b - a:.4f},asetpts=PTS-STARTPTS,"
                 f"adelay={ms}|{ms},volume={it.get('volume_db', 0)}dB[ia{j}]")
        extra.append(f"[ia{j}]")
        nin += 1
    lbl, nin = _sfx_graph(plan, inputs, g, nin)
    extra += lbl
    mix = ["[acat]"]
    bgm = None if "bgm" in plan.mute else d.get("bgm")
    if bgm:
        inputs += ["-stream_loop", -1, "-i", proj.resolve(bgm["file"])]
        fade = min(2.0, plan.total / 4)
        g.append(f"[{nin}:a]atrim=0:{plan.total:.4f},asetpts=PTS-STARTPTS,volume={bgm.get('volume_db', -23.5)}dB,"
                 f"afade=t=out:st={plan.total - fade:.3f}:d={fade:.3f}[bgm]")
        if bgm.get("duck"):
            g.append("[acat]asplit=2[vo][sc]")
            g.append("[bgm][sc]sidechaincompress=threshold=0.03:ratio=8:attack=20:release=400[bgmd]")
            mix = ["[vo]", "[bgmd]"]
        else:
            mix.append("[bgm]")
    mix += extra
    if len(mix) == 1:
        return inputs, ";\n".join(g), "acat"
    g.append(f"{''.join(mix)}amix=inputs={len(mix)}:duration=first:normalize=0,alimiter=limit=0.841:attack=5:release=50:level=0[amix]")  # 最後に -1.5dB で頭打ち(効果音の重なりで割れないように。AAC で +0.1〜0.5dB はみ出すので -1dBTP に余裕を持たせる)
    return inputs, ";\n".join(g), "amix"


CHUNK_SEC = 60.0


def _cpu_cores(proj):
    """書き出しに使ってよい論理コア数(output.cpu_percent。既定50%)。全部埋めると PC が操作できなくなる。"""
    import os
    n = os.cpu_count() or 2
    pct = float(proj.data["output"].get("cpu_percent") or 50)
    return n, max(2, min(n, round(n * pct / 100)))


def _limit_cores(pid, n, k):
    """ffmpeg を後ろ側の k 個の論理コアだけで動かす。"""
    if k >= n:
        return
    try:
        import psutil
        psutil.Process(pid).cpu_affinity(list(range(n - k, n)))
    except Exception:
        pass
SEEK_GAP = 8.0  # 分割書き出しで、元の時刻がこれ以上離れた区間は元の動画を開き直してシークする(間をデコードしない)
# 分割書き出しで元の動画を NVDEC で読むか。測ると CPU で読む方が速く、CPU 時間も少ない(GPU から戻す nv12 の変換が重い)ので使わない
GPU_DEC = False


_TAGS = {}


def _base_norm(src):
    """背景(V1)の形式をそろえるフィルタ。色の情報が無い元動画は bt709/tv と書くだけにする。
    書かないと bt709 の差し込み動画・テロップと合わせるため ffmpeg が毎コマ色空間の変換を挟み、1コマ数十 ms かかっていた
    (絵は変わらない。書き出した動画もビット単位で同じ)"""
    import os
    try:
        key = (str(src), os.stat(src).st_mtime_ns)
    except OSError:
        key = (str(src), 0)
    if key not in _TAGS:
        try:
            out = subprocess.run([ff.find("ffprobe"), "-v", "error", "-select_streams", "v:0", "-show_entries",
                                  "stream=color_range,color_space,color_primaries,color_transfer", "-of", "json", str(src)],
                                 capture_output=True, text=True, encoding="utf-8", creationflags=ff.NOWIN).stdout
            s = (json.loads(out).get("streams") or [{}])[0]
        except Exception:
            s = {}
        sp = []
        if s.get("color_range") in (None, "unknown"):
            sp.append("range=tv")
        if s.get("color_space") in (None, "unknown"):
            sp.append("colorspace=bt709")
        _TAGS[key] = "format=yuv420p" + (",setparams=" + ":".join(sp) if sp else "")
        _BASE_CS[key[0]] = (_known(s.get("color_range")) or "tv", _known(s.get("color_space")) or "bt709")
        _BASE_PT[key[0]] = (_known(s.get("color_primaries")), _known(s.get("color_transfer")))
    return _TAGS[key]


_BASE_CS = {}
_BASE_PT = {}  # 元の動画の color_primaries / color_trc(自前エンジンの出力に書く)


def render_chunked(proj, out, draft=False, on_progress=None, workers=None):
    """長い・差し込みの多いプロジェクトの書き出し(Kdenlive/Premiere のプレビューファイル方式)。
    映像を約60秒ずつ「その範囲に出る素材だけ」の小さなグラフで並列に書き出し、音声は1本で作って最後に無劣化でつなぐ。
    1本の巨大なグラフだと入力が数百になり、メモリ数十GB・速度が1/30 になるため。
    チャンクはグラフの内容でキャッシュするので、直した範囲だけ作り直しになる。"""
    import hashlib, subprocess, threading
    from concurrent.futures import ThreadPoolExecutor
    plan = Plan(proj)
    plan.draft = draft  # エフェクトも小さく焼く(fx.Elem)
    ncpu, cores = _cpu_cores(proj)
    if workers is None:  # 同時に書き出す区間の数(1本あたり 3〜4 コアくらい使う)
        workers = 4 if cores >= ncpu - 1 else max(1, min(3, cores // 4))  # 放置用(95%)は4本
    cache = proj.cache_dir
    cdir = cache / "chunks"
    cdir.mkdir(exist_ok=True)
    fps, total = plan.fps, plan.total
    # 区切りは60秒ごと。近くのカットのつなぎ目(±5秒)があればそこへ寄せ、フレーム境界にそろえる
    joints, acc = [], 0.0
    for pc in plan.pieces:
        acc += pc[1] if pc[0] == "gap" else pc[2] - pc[1]
        joints.append(acc)
    bounds, t = [0], CHUNK_SEC
    while t < total - CHUNK_SEC / 3:
        near = [j for j in joints if abs(j - t) <= 5]
        x = min(near, key=lambda j: abs(j - t)) if near else t
        bounds.append(round(x * fps))
        t = x + CHUNK_SEC
    bounds.append(round(total * fps))
    vcodec = draft_codec_args(proj.data["output"]) if draft else video_codec_args(proj.data["output"])
    accel = gpu_accel(proj)
    if accel:  # GPU 合成は1区間あたり VRAM を 0.8GB ほど使う。空きが足りなければ CPU で(ゲームなどで GPU を使っているとき)
        free = ff.vram_free_mib()
        if free is not None and free < VRAM_PER_CHUNK * workers:
            print(f"VRAM の空きが {free}MiB しかないので、重ね合わせは CPU でします", flush=True)
            accel = False
    jobs, subs_written = [], False
    # 区間の作り方を順に試す: 自前エンジン → ffmpeg(GPU 合成)→ ffmpeg(CPU)。前のが失敗したら次でやり直す
    modes = (["engine"] if engine_on(proj) else []) + (["gpu"] if accel else []) + ["cpu"]

    def chunk_args(k, mode):
        """区間 k を mode で作る → (コマンド(ffmpeg なら None), ffmpeg の引数, キャッシュの鍵の元, 字幕)。
        自前エンジンで作る意味がない(差し込みがない)区間は None"""
        A, B = bounds[k] / fps, bounds[k + 1] / fps
        nfr = bounds[k + 1] - bounds[k]
        if mode == "engine":
            lay = []
            inputs, graph, vl, _, subs = build(plan, want_audio=False, draft=draft, win=(A, B), gpu=GPU_DEC, layers=lay)
            if not lay:  # 差し込みのない区間は ffmpeg のままで同じ
                return None
            (cdir / f"b{k:03d}.txt").write_text(graph, encoding="utf-8")
            job = engine_job(plan, k, bounds, draft, vcodec, inputs, vl, lay, subs, min(4, cores))
            jf = cdir / f"e{k:03d}.json"
            jf.write_text(json.dumps(job, ensure_ascii=False, indent=1), encoding="utf-8")
            return ("engine", jf), [], graph + json.dumps(job, ensure_ascii=False), subs
        inputs, graph, vl, _, subs = build(plan, want_audio=False, draft=draft, win=(A, B), gpu=GPU_DEC, accel=mode == "gpu")
        args = [*map(str, inputs), "-filter_complex_threads", str(min(4, cores)), "-filter_complex_script", f"chunks/g{k:03d}.txt", "-map", f"[{vl}]",
                "-frames:v", str(nfr), *map(str, vcodec), "-an"]
        (cdir / f"g{k:03d}.txt").write_text(graph, encoding="utf-8")
        return None, args, graph + "|" + "|".join(args), subs

    for k in range(len(bounds) - 1):
        for mi, mode in enumerate(modes):
            r = chunk_args(k, mode)
            if r:
                break
        cmd, args, keysrc, subs = r
        if subs and not subs_written:
            (cache / "subs.ass").write_text(subs, encoding="utf-8")
            subs_written = True
        nfr = bounds[k + 1] - bounds[k]
        key = hashlib.sha1((keysrc + "|" + (subs or "")).encode("utf-8")).hexdigest()[:12]
        jobs.append((k, cdir / f"c{k:03d}_{key}.mp4", (cmd, args), nfr, modes[mi:]))
    keep = {j[1].name for j in jobs}
    for old in cdir.glob("c*.mp4"):
        if old.name not in keep:
            old.unlink(missing_ok=True)
    done = {j[0]: (j[3] if j[1].exists() else 0) for j in jobs}
    allf = sum(j[3] for j in jobs) or 1
    lock = threading.Lock()

    def report():
        if on_progress:
            on_progress(min(0.97, sum(done.values()) / allf * 0.97))

    report()

    names = {"engine": "自前エンジン", "gpu": "ffmpeg(GPU 合成)", "cpu": "ffmpeg(CPU)"}

    def run(job):
        k, f, (cmd, args), nfr, (mode, *rest) = job
        if f.exists():
            return
        while True:
            try:
                return encode(k, f, args, nfr, cmd)
            except RuntimeError as e:
                if not rest:
                    raise
                # 失敗した区間(自前エンジンの不具合・GPU の VRAM 不足・色空間の違いなど)は次の作り方でやり直す
                print(f"chunk {k + 1}: {names[mode]}で失敗したので {names[rest[0]]} でやり直します: {str(e)[-300:]}", flush=True)
                with lock:
                    done[k] = 0
                mode, rest = rest[0], rest[1:]
                with build_lock:
                    cmd, args = chunk_args(k, mode)[:2]

    build_lock = threading.Lock()

    srv = []  # 自前エンジン(最初に使う区間で起こす)

    def engine(k, f, jf, nfr):
        with build_lock:
            if not srv:
                srv.append(_EngineServer(cache, ncpu, cores))
        tmp = f.with_suffix(".tmp.mp4")
        q = srv[0].submit(k, jf, tmp)
        while True:
            msg = q.get()
            if msg.startswith("frame="):
                with lock:
                    done[k] = min(nfr, int(msg[6:]))
                report()
            elif msg == "ok":
                break
            else:
                tmp.unlink(missing_ok=True)
                raise RuntimeError(f"自前エンジンが失敗しました(区間{k}): {msg[4:][-600:]}")
        tmp.replace(f)
        with lock:
            done[k] = nfr
        print(f"chunk {k + 1}/{len(jobs)} done(自前エンジン)", flush=True)

    def encode(k, f, args, nfr, cmd=None):
        if cmd:
            return engine(k, f, cmd[1], nfr)
        tmp = f.with_suffix(".tmp.mp4")
        p = subprocess.Popen([ff.find("ffmpeg"), "-hide_banner", "-y", "-loglevel", "error", "-nostdin", "-progress", "pipe:1",
                              "-nostats", *args, str(tmp)], cwd=cache, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             text=True, encoding="utf-8", errors="replace",
                             creationflags=ff.NOWIN | getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0))  # 並列でCPUを埋めるので、他の作業を邪魔しない優先度
        _limit_cores(p.pid, ncpu, cores)
        err = []
        th = threading.Thread(target=lambda: err.append(p.stderr.read()), daemon=True)
        th.start()
        for line in p.stdout:
            if line.startswith("frame="):
                try:
                    with lock:
                        done[k] = min(nfr, int(line.split("=")[1]))
                    report()
                except ValueError:
                    pass
        code = p.wait()
        th.join(5)
        if code:
            raise RuntimeError(f"ffmpeg が失敗しました(区間{k}): {''.join(err)[-600:]}")
        tmp.replace(f)
        with lock:
            done[k] = nfr
        print(f"chunk {k + 1}/{len(jobs)} done", flush=True)

    # 音声(1本)。映像のチャンクと並行して作る
    ainputs, agraph, al = build_audio(plan)
    (cdir / "audio_graph.txt").write_text(agraph, encoding="utf-8")
    akey = hashlib.sha1((agraph + "|".join(map(str, ainputs)) + "|" + _aac_kbps(proj)).encode("utf-8")).hexdigest()[:12]
    afile = cdir / f"audio_{akey}.m4a"
    for old in cdir.glob("audio_*.m4a"):
        if old != afile:
            old.unlink(missing_ok=True)

    def run_audio():
        if afile.exists():
            return
        tmp = afile.with_suffix(".tmp.m4a")
        ff.run([*ainputs, "-filter_complex_script", "chunks/audio_graph.txt", "-map", f"[{al}]", "-t", f"{total:.3f}",
                "-c:a", "aac", "-b:a", _aac_kbps(proj), str(tmp)], cwd=cache)
        tmp.replace(afile)
        print("audio done", flush=True)

    try:
        with ThreadPoolExecutor(workers + 1) as ex:
            futs = [ex.submit(run_audio)] + [ex.submit(run, j) for j in jobs]
            for fu in futs:
                fu.result()
    finally:
        if srv:
            srv[0].close()
    lst = cdir / "list.ffconcat"
    lst.write_text("ffconcat version 1.0\n" + "".join(f"file '{j[1].name}'\n" for j in jobs), encoding="utf-8")
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    ff.run(["-f", "concat", "-safe", 0, "-i", lst, "-i", afile, "-map", "0:v", "-map", "1:a", "-c", "copy",
            "-t", f"{total:.3f}", "-movflags", "+faststart", str(Path(out).resolve())], cwd=cache)
    res = {"output": str(Path(out).resolve()), "duration": round(total, 2), "segments": len(plan.keeps), "chunks": len(jobs)}
    try:  # 書き出し後の見落としチェック(効果音の抜け・重なり・音量/ピーク/長さ)。失敗しても書き出しは成功扱い
        from . import check
        res["checks"] = check.run(proj, plan)
        if not draft:
            res["audio"], more = check.verify_output(proj, res["output"], total)
            res["checks"] += more
    except Exception as e:
        res["checks"] = [{"level": "info", "at": None, "msg": f"チェックできませんでした: {e}"}]
    if on_progress:
        on_progress(1.0)
    return res


def engine_job(plan, k, bounds, draft, vcodec, inputs, vl, layers, subs, threads):
    """自前エンジン(engine.py)への区間 k の指示。背景は build(layers=[]) のグラフ(chunks/b{k}.txt)を ffmpeg で作る"""
    fps, nfr = plan.fps, bounds[k + 1] - bounds[k]
    cs = _BASE_CS.get(str(plan.src), ("tv", "bt709"))
    # 生の絵には色の情報が付かないので、ffmpeg の書き出しと同じ色の情報を書く(テロップの色もこれで決まる)
    pt = zip(("color_primaries", "color_trc"), _BASE_PT.get(str(plan.src), ()))
    sp = f"setparams=range={cs[0]}:colorspace={cs[1]}" + "".join(f":{n}={v}" for n, v in pt if v)
    vf = f"setpts=PTS+{bounds[k] / fps:.6f}/TB,{sp}"
    if subs:
        vf += ",ass=subs.ass"
    vf += f",setpts=PTS-STARTPTS,{_out_scale(plan, draft)}format=yuv420p"
    return {"ffmpeg": ff.find("ffmpeg"), "cwd": str(plan.proj.cache_dir), "W": plan.W, "H": plan.H, "F": fps, "rate": str(fps),
            "N": nfr, "m": bounds[k], "gpu": True,
            "base": [*map(str, inputs), "-filter_complex_threads", str(threads), "-filter_complex_script", f"chunks/b{k:03d}.txt",
                     "-map", f"[{vl}]", "-frames:v", str(nfr)],
            "enc": ["-vf", vf, "-frames:v", str(nfr), *map(str, vcodec), "-an"], "layers": layers}


VENGINE = Path(__file__).resolve().parent.parent / "vengine" / "dist" / "vengine.exe"


def engine_cmd(jf):
    """自前エンジンを動かすコマンド(出力ファイルは後ろに足す)。Rust 版(vengine.exe)があればそれ、なければ engine.py。
    engine.py は pythonw で動いていても python.exe で(画面は出さない)"""
    import sys
    if VENGINE.exists() and not os.environ.get("VEDIT_PYENGINE"):
        return [str(VENGINE), str(jf)]
    exe = Path(sys.executable)
    if exe.name.lower() == "pythonw.exe" and exe.with_name("python.exe").exists():
        exe = exe.with_name("python.exe")
    return [str(exe), "-u", str(Path(__file__).with_name("engine.py")), str(jf)]


class _EngineServer:
    """書き出し1回ぶんだけ常駐する engine.py(torch の読み込み・CUDA の起動 2〜7秒を区間ごとに払わない)。
    区間は submit で頼み、messages(k) の行("frame=N" / "ok" / "err ...")で結果を受け取る"""

    def __init__(self, cwd, ncpu, cores):
        import queue, subprocess, threading
        self.qs, self.lock, self.dead = {}, threading.Lock(), None
        self._queue = queue.Queue
        self.p = subprocess.Popen(engine_cmd("--serve"), cwd=cwd, env={**os.environ, "PYTHONIOENCODING": "utf-8"}, stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace",
                                  creationflags=ff.NOWIN | getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0))
        _limit_cores(self.p.pid, ncpu, cores)
        self.err = []
        threading.Thread(target=lambda: self.err.append(self.p.stderr.read()), daemon=True).start()
        threading.Thread(target=self._read, daemon=True).start()

    def _q(self, k):
        with self.lock:
            return self.qs.setdefault(k, self._queue())

    def _read(self):
        for line in self.p.stdout:
            k, _, msg = line.rstrip("\n").partition(" ")
            if k.isdigit():
                self._q(int(k)).put(msg)
        # 落ちたら待っている区間すべてに知らせる(ffmpeg でやり直させる)
        self.dead = "自前エンジンが止まりました: " + "".join(self.err)[-600:]
        with self.lock:
            for q in self.qs.values():
                q.put("err " + self.dead)

    def submit(self, k, jf, out):
        q = self._q(k)
        while not q.empty():
            q.get_nowait()
        if self.dead:
            raise RuntimeError(self.dead)
        try:
            with self.lock:
                self.p.stdin.write(json.dumps({"k": k, "job": str(jf), "out": str(out)}) + "\n")
                self.p.stdin.flush()
        except OSError as e:
            raise RuntimeError(f"自前エンジンに渡せませんでした: {e}")
        return q

    def close(self):
        try:
            self.p.stdin.close()
        except OSError:
            pass
        try:
            self.p.wait(30)
        except Exception:
            self.p.kill()


def _aac_kbps(proj):
    """音声のビットレート。ffmpeg 標準の AAC は 288k 以上で鋭い立ち上がりの音を +3dB ほど飛び出させる(0dB を超えて割れる)ので
    256k までにする(2026-10-10 実作業: 320k で 8:05 にピーク +2.6dB。256k なら -1dB に収まる。YouTube は再エンコードするので差は出ない)"""
    return f'{min(256, int(proj.data["output"].get("audio_kbps") or 192))}k'


def build(plan, want_audio=True, draft=False, ass_name="subs.ass", at=None, win=None, gpu=False, accel=False, layers=None):
    """(入力引数, filter_complex文字列, 映像ラベル, 音声ラベル|None, 字幕テキスト) を返す。
    at=秒 を渡すと静止画用に、その時刻の付近だけをシークして作る(長尺でも速い)。
    win=(A,B) は出力の A〜B 秒だけの映像(音声なし。分割書き出し用。時刻は全体のまま、出口で0からにする)。
    gpu=True(win のときだけ): 元の動画の読み込みを NVDEC に任せ、差し込み動画のデコードは1本2スレッドまでにする。
    (差し込みも全部 GPU にすると、入力が数十本ある区間で VRAM 8GB を使い切って PC ごと固まった。
     CPU のままスレッド数を絞らないと、入力ごとにコア数ぶんのスレッドができて千本を超える)
    accel=True(win のときだけ): 差し込み・画像の重ね合わせを GPU(overlay_cuda)でする。画像は1枚だけ GPU へ送って GPU の中で繰り返し、
    全画面の差し込み動画は NVDEC で読んで GPU のまま縮小する(1区間 NVDEC_MAX 本まで。残りは CPU で読んで送る)。
    CUDA の器(デバイス)は1つを使い回す(フィルタごとに作ると VRAM を食う)。テロップ(ASS)の前に CPU へ戻す
    layers=[](win のときだけ): 自前エンジン(engine.py)用。差し込み・テロップ・縮小を入れない背景だけのグラフにし、
    差し込みは engine.py への指示(_engine_layer)にして layers に入れる"""
    proj, d = plan.proj, plan.proj.data
    wav = proj.cache_dir / "audio.wav"
    inputs = ["-i", plan.src, "-i", wav if wav.exists() else plan.src]
    P = plan.pieces
    fp = _frame_piece(plan, at) if at is not None and not want_audio else None
    if win:
        want_audio = False
        P, seek = _win_pieces(plan, *win)
        if layers is not None:  # 自前エンジン: 差し込みを先に決め、全画面の不透明な動画に丸ごと隠れる元動画の区間は読まずに黒にする
            ovs_e = [o for o in plan.ovs_out(when=win) if o[0] < win[1] and o[1] > win[0]]
            layers += [_engine_layer(plan, a, b, it, win) for a, b, it in ovs_e]
            P = _hide_pieces(plan, P, win, layers)
        inputs = ["-ss", f"{seek:.4f}", *(["-hwaccel", "cuda"] if gpu else []), *inputs]
    if fp:
        P = [fp[0]]
        inputs = ["-ss", f"{fp[2]:.4f}", *inputs]
    n = len(P)
    g = []

    # 1) カット = 残す区間を連結。空白(gap)は黒+無音を挟む
    vs = "".join(f"[v{i}]" for i in range(n))
    # つなぎ目の処理: 音は短いフェードでブツ切れ音を防ぐ。映像は任意で暗転/白飛びを挟む(尺は変わらない)
    fx = {"audio_fade_ms": 8, "video": "none", "dur": 0.12, **(d.get("cut_fx") or {})}
    colors = {"dip-black": "black", "dip-white": "white"}
    has_gap = any(p[0] == "gap" for p in P)
    # 空白と連結するときは形式をそろえる
    vnorm = ",format=yuv420p,setsar=1" if has_gap else ""
    anorm = ",aformat=sample_fmts=fltp:sample_rates=48000:channel_layouts=stereo" if has_gap else ""
    o, used = 0.0, []  # win: 出力のフレーム位置に合わせて区間ごとのコマ数をぴったりにする(つなぎ目ごとの半コマのずれが積もらない)
    src_in, nsrc = {}, 0  # win: 区間ごとの (入力番号, その入力のシーク位置)
    if win:  # 元の時刻が SEEK_GAP 秒以上離れた区間は、同じ元動画をシークして開き直す
        # (1本の入力から全部切り出すと、カットで捨てる部分まで頭から全部デコードする。60秒を作るのに数百秒ぶん読んでいた)
        last, cin, off = None, 0, 0.0
        for i, pc in enumerate(P):
            if pc[0] == "gap":
                continue
            if last is not None and (pc[1] < last - 1e-3 or pc[1] - last > SEEK_GAP):
                nsrc += 1
                cin, off = 1 + nsrc, max(0.0, pc[1] - 1.0)
                extra = ["-ss", f"{seek + off:.4f}", *(["-hwaccel", "cuda"] if gpu else []), "-i", plan.src]
                inputs += extra
            src_in[i], last = (cin, off), pc[2]
    for i, pc in enumerate(P):
        if win:
            L = pc[1] if pc[0] == "gap" else pc[2] - pc[1]
            k0, k1 = math.ceil(o * plan.fps - 1e-6), math.ceil((o + L) * plan.fps - 1e-6)
            sh, o = k0 / plan.fps - o, o + L
            if k1 <= k0:
                continue
            used.append(f"[v{i}]")
            if pc[0] == "gap":
                g.append(f"color=c=black:s={plan.W}x{plan.H}:r={plan.fps},trim=end_frame={k1 - k0}{vnorm}[v{i}]")
            else:
                si, off = src_in[i]
                g.append(f"[{si}:v]trim=start={pc[1] - off + sh:.4f}:end={pc[2] - off + 0.1:.4f},setpts=PTS-STARTPTS,fps={plan.fps},"
                         f"tpad=stop_mode=clone:stop=-1,trim=end_frame={k1 - k0},setpts=PTS-STARTPTS{vnorm}[v{i}]")
            continue
        if pc[0] == "gap":
            L = pc[1]
            g.append(f"color=c=black:s={plan.W}x{plan.H}:r={plan.fps}:d={L:.4f}{vnorm}" + (f",setpts=PTS-STARTPTS+{fp[1]:.4f}/TB" if fp else "") + f"[v{i}]")
            if want_audio:
                g.append(f"anullsrc=r=48000:cl=stereo,atrim=0:{L:.4f},asetpts=PTS-STARTPTS{anorm}[a{i}]")
            continue
        _, a, b = pc
        L, first, last = b - a, i == 0, i == n - 1
        vf = ""
        if fx["video"] in colors:
            dur = min(float(fx["dur"]), L / 3)
            if not first:
                vf += f",fade=t=in:st=0:d={dur:.3f}:color={colors[fx['video']]}"
            if not last:
                vf += f",fade=t=out:st={L - dur:.3f}:d={dur:.3f}:color={colors[fx['video']]}"
        if fp:  # 入力側でシーク済み。出力上の時刻に合わせる
            g.append(f"[0:v]trim=start=0:end={b - a:.4f},setpts=PTS-STARTPTS+{fp[1]:.4f}/TB,fps={plan.fps}{vnorm}[v{i}]")
            continue
        g.append(f"[0:v]trim=start={a:.4f}:end={b:.4f},setpts=PTS-STARTPTS,fps={plan.fps}{vf}{vnorm}[v{i}]")
        if want_audio:
            af = ""
            fade = min(fx["audio_fade_ms"] / 1000, L / 3)
            if fade > 0 and n > 1:
                if not first:
                    af += f",afade=t=in:st=0:d={fade:.4f}"
                if not last:
                    af += f",afade=t=out:st={L - fade:.4f}:d={fade:.4f}"
            g.append(f"[1:a]atrim=start={a:.4f}:end={b:.4f},asetpts=PTS-STARTPTS{af}{anorm}[a{i}]")
    cut = _cut_wav(proj, P, fx) if want_audio and not fp and wav.exists() and not plan.slips else None  # 音声のずらしは元の wav を使う
    if win:
        vs, n = "".join(used), len(used)
    if cut:  # 音声は先に1本のwavへ切ってつないである(数百か所を filter で切ると何倍も遅い)
        inputs[inputs.index(wav)] = cut
        g[:] = [x for x in g if not x.startswith(("[1:a]", "anullsrc"))]
        g.append(f"{vs}concat=n={n}:v=1:a=0[vcat0]")
        g.append("[1:a]aformat=sample_fmts=fltp:sample_rates=48000:channel_layouts=stereo[acat0]")
    elif want_audio:
        pairs = "".join(f"[v{i}][a{i}]" for i in range(n))
        g.append(f"{pairs}concat=n={n}:v=1:a=1[vcat0][acat0]")
    elif fp:
        g.append("[v0]null[vcat0]")
    else:
        g.append(f"{vs}concat=n={n}:v=1:a=0[vcat0]")
    en = lambda rs: "+".join(f"between(t,{a:.4f},{b:.4f})" for a, b in rs)
    if win:  # 出力全体の時刻にそろえる(差し込み・テロップ・黒の時刻をそのまま使える)
        g.append(f"[vcat0]setpts=PTS-STARTPTS+{win[0]:.4f}/TB[vcatw]")
        base = "vcatw"
    else:
        base = "vcat0"
    base = vfx.v1_graph(plan, g, base, win=win, at=at if fp else None)  # 背景(V1)の揺れ
    nb = _base_norm(plan.src)  # 重ね合わせ(overlay format=yuv420)の下地。透明度は持たない
    g.append(f"[{base}]{nb},drawbox=x=0:y=0:w=iw:h=ih:color=black:t=fill:enable='{en(plan.black)}'[vcat]" if plan.black else f"[{base}]{nb}[vcat]")
    if want_audio:
        g.append(f"[acat0]volume=0:enable='{en(plan.silent)}'[acat]" if plan.silent else "[acat0]anull[acat]")
    cur, nin = "vcat", 2 + nsrc
    gpu = bool(gpu and win)
    accel = bool(accel and win)
    base_cs = _BASE_CS.get(str(plan.src), ("tv", "bt709"))
    on_gpu = hw = False  # on_gpu: 今の絵が GPU にあるか / hw: この差し込み動画を NVDEC で読むか
    nvdec = 0

    # 2) 映像・画像の差し込み
    extra_audio = []
    if want_audio:  # リンク解除で動かした音声
        for k, (a, b, o) in enumerate(plan.slips):
            ms, fd = int(round(o * 1000)), min(0.008, (b - a) / 3)
            g.append(f"[1:a]atrim=start={a:.4f}:end={b:.4f},asetpts=PTS-STARTPTS,afade=t=in:d={fd:.4f},afade=t=out:st={b - a - fd:.4f}:d={fd:.4f},"
                     f"adelay={ms}:all=1[sl{k}]")
            extra_audio.append(f"[sl{k}]")
    if want_audio:  # エフェクトで焼き直す動画の音は元の素材から
        for k, (a, b, it) in enumerate(plan.conv(plan.overlays())):
            if not (vfx.needs_bake(it) and it.get("audio") and it.get("file")):
                continue
            f = proj.resolve(it["file"])
            if f.suffix.lower() in IMG_EXT or not ff.probe(f)["has_audio"]:
                continue
            inputs += ["-i", f]
            ms = int(a * 1000)
            g.append(f"[{nin}:a]atrim=start={parse_time(it.get('in', 0))}:duration={b - a:.4f},asetpts=PTS-STARTPTS,"
                     f"adelay={ms}|{ms},volume={it.get('volume_db', 0)}dB[fa{k}]")
            extra_audio.append(f"[fa{k}]")
            nin += 1
    ovs = plan.ovs_out(when=(at, at + 1 / plan.fps) if fp else win)
    if fp:  # 静止画: その時刻に出ている差し込みだけ(at がコマの間なら次のコマが出るので、1コマぶん見る)
        ovs = [o for o in ovs if o[0] < at + 1 / plan.fps and o[1] > at]
    if win:  # その範囲に出ているものだけ
        ovs = [o for o in ovs if o[0] < win[1] and o[1] > win[0]]
    if win and layers is not None:  # 重ね合わせは自前エンジンで(layers は上で作ってある)
        ovs = []
    vtracks, ovs = ([], ovs) if fp or win else _video_tracks(ovs, plan)
    vk = {id(t[0][2]): t for t in vtracks}
    tracks, ovs = ([], ovs) if fp or win else _image_tracks(ovs, plan)
    tk = {id(t[0][2]): t for t in tracks}  # トラックは最初の要素の位置(重なり順)で合成する
    # 差し込み動画の色情報。書いていない動画は下地と同じと書く(書かないと CPU の overlay が BT.601 とみなして変換し、色が少しずれていた)
    cfix = {}
    for a, b, it in ovs:
        f = proj.resolve(it["file"])
        if f.suffix.lower() not in IMG_EXT:
            cfix[id(it)] = _color_fix(f, base_cs)
    if any(v is None for v in cfix.values()):  # 本当に色の変換が要る差し込みがある区間は CPU で重ねる(CPU の overlay が変換する)
        accel = False
    for j, (a, b, it) in enumerate(ovs):
        if accel and not on_gpu:  # 下の絵を GPU へ(以降の重ね合わせは overlay_cuda)
            inputs[:0] = ["-init_hw_device", "cuda=gpu", "-filter_hw_device", "gpu"]
            g.append(f"[{cur}]hwupload[gup]")
            cur, on_gpu = "gup", True
        up = ",hwupload" if on_gpu else ""
        ov0 = "overlay_cuda=0:0:eof_action=pass" if on_gpu else "overlay=0:0:eof_action=pass:format=yuv420"
        if id(it) in vk:  # 続けて並ぶ動画(1本にまとめたもの)
            nin = video_track_graph(proj, plan, vk[id(it)], inputs, g, nin, f"o{j}")
            if on_gpu:
                g.append(f"[o{j}]setparams=range={base_cs[0]}:colorspace={base_cs[1]},hwupload[o{j}u]")
            g.append(f"[{cur}][o{j}{'u' if on_gpu else ''}]{ov0}[ov{j}]")
            cur = f"ov{j}"
            continue
        if id(it) in tk:
            lst = cache_track(proj, plan, tk[id(it)], j)
            inputs += ["-f", "concat", "-safe", 0, "-i", lst]
            g.append(f"[{nin}:v]fps={plan.fps},format=yuva420p{up}[o{j}]")
            g.append(f"[{cur}][o{j}]{ov0}[ov{j}]")
            cur, nin = f"ov{j}", nin + 1
            continue
        f = proj.resolve(it["file"])
        is_img = f.suffix.lower() in (".png", ".jpg", ".jpeg", ".webp", ".bmp")
        if is_img:  # 1枚だけデコードして、縮小した後にフィルタの中で繰り返す
            # (-loop 1 だと入力ごとに先読みのフレームが溜まり、画像が数百枚あると数十GBになる)
            inputs += [*(["-threads", 1] if gpu else []), "-i", _small_image(proj, plan, it, f)]
            pre = None
        else:
            skip = max(0.0, win[0] - a) if win else 0.0
            hw = on_gpu and nvdec < NVDEC_MAX and _gpu_video_ok(it, plan, f) and _nvdec_ok(f)
            if hw:
                inputs += ["-hwaccel", "cuda", "-hwaccel_device", "gpu", "-hwaccel_output_format", "cuda"]
                nvdec += 1
            if gpu:
                inputs += ["-threads", 2]
            if skip:  # 範囲の前から出ている動画は途中から(入力側でシーク)
                # 1コマ手前から読み、コマの時刻はそのまま使う(ちょうど skip でシークすると、範囲の頭に出ているはずのコマが
                # 捨てられて1コマ先の絵が出る。PTS-STARTPTS だとコマの時刻の端数も消える)
                ss = max(0.0, parse_time(it.get('in', 0)) + skip - 1 / plan.fps)
                inputs += ["-ss", f"{ss:.4f}", "-i", f]
                pre = f"setpts=PTS+{a + ss - parse_time(it.get('in', 0)):.4f}/TB,tpad=stop_mode=clone:stop_duration=0.5"
            else:
                inputs += ["-i", f]
                pre = f"trim=start={parse_time(it.get('in', 0))},setpts=PTS-STARTPTS+{a:.4f}/TB,tpad=stop_mode=clone:stop_duration=0.5"
        # 出入りアニメーション: fade は透明度、slide-* は位置で表現
        dur = min(float(it.get("anim_dur", 0.3)), (b - a) / 2)
        ain, aout_ = it.get("in_anim", "none"), it.get("out_anim", "none")
        fade = ""
        if ain == "fade":
            fade += f",fade=t=in:st={a:.4f}:d={dur:.3f}:alpha=1"
        if aout_ == "fade":
            fade += f",fade=t=out:st={b - dur:.4f}:d={dur:.3f}:alpha=1"
        if pre is None:
            a0 = max(a, win[0]) if win else a
            nf = round(((min(b, win[1]) if win else b) - a0) * plan.fps) + 1
            # GPU のときは1枚だけ送って GPU の中で繰り返す(フェードは CPU で描くので、そのときは毎コマ送る)
            g.append(f"[{nin}:v]{_scale_filter(it, plan)},format=yuva420p{'' if fade else up},loop=loop={nf - 1}:size=1:start=0,settb=1/60000,"  # 画像の時間単位は1/25秒なので細かくしてから時刻を振る
                     f"setpts=N/{plan.fps}/TB+{a0:.4f}/TB{fade}{up if fade else ''}[o{j}]")
        elif hw:
            g.append(f"[{nin}:v]{pre},scale_cuda={plan.W}:{plan.H}:format=yuv420p:interp_algo=lanczos{cfix[id(it)]}[o{j}]")
        else:
            g.append(f"[{nin}:v]{pre},{_scale_filter(it, plan)},format=yuva420p{cfix.get(id(it)) or ''}{fade}{up}[o{j}]")
        xs, ys = _overlay_pos(it, plan).split(":")
        p_in = f"(1-min(1,max(0,(t-{a:.4f})/{dur:.3f})))"
        p_out = f"min(1,max(0,(t-{b - dur:.4f})/{dur:.3f}))"
        for kind, p in ((ain, p_in), (aout_, p_out)):
            if kind == "slide-left":
                xs = f"({xs})-{p}*(({xs})+w)"
            elif kind == "slide-right":
                xs = f"({xs})+{p}*(W-({xs}))"
            elif kind == "slide-up":
                ys = f"({ys})+{p}*(H-({ys}))"
            elif kind == "slide-down":
                ys = f"({ys})-{p}*(({ys})+h)"
        ovf = "overlay_cuda" if on_gpu else "overlay"
        if on_gpu:  # CPU の overlay(format=yuv420)は位置を偶数に切り捨てる。overlay_cuda は切り捨てないので合わせる(1px ずれる)
            xs, ys = (f"2*floor(trunc({v})/2)" for v in (xs, ys))
        g.append(f"[{cur}][o{j}]{ovf}=x='{xs}':y='{ys}':enable='between(t,{a:.4f},{b:.4f})':eof_action=pass{'' if on_gpu else ':format=yuv420'}[ov{j}]")
        if want_audio and it.get("audio") and not is_img and ff.probe(f)["has_audio"]:
            ms = int(a * 1000)
            vol = it.get("volume_db", 0)
            g.append(f"[{nin}:a]atrim=start={parse_time(it.get('in', 0))}:duration={b - a:.4f},asetpts=PTS-STARTPTS,"
                     f"adelay={ms}|{ms},volume={vol}dB[ia{j}]")
            extra_audio.append(f"[ia{j}]")
        cur, nin = f"ov{j}", nin + 1

    if want_audio:  # 効果音(処理済み音声のノーマライズとは別に、最後のミックスで足す)
        lbl, nin = _sfx_graph(plan, inputs, g, nin)
        extra_audio += lbl

    if on_gpu:  # テロップは CPU で描くので戻す
        g.append(f"[{cur}]hwdownload,format=yuv420p[gdown]")
        cur = "gdown"

    # 3) テロップ + トピックバー(ASS)
    shown = lambda k, lane: [] if lane in plan.hide else [t for t in d[k] if str(t.get("text", "")).strip()]  # 空の文字・非表示のトラックは出さない
    mhide = vfx.matte_map(d)[1]  # マット(型抜き)に使うテロップは出さない
    telops = plan.conv([t for t in shown("telops", "t") if not vfx.needs_bake_telop(t) and id(t) not in mhide])  # 動きを付けたテロップは fx.py で焼いて差し込みに
    topics = plan.conv(shown("topics", "tb"))
    subs = ass.build(proj, plan.W, plan.H, telops, topics) if (telops or topics) else None
    if subs and not (win and layers is not None):  # 自前エンジンのときは合成した後に engine.py が足す
        g.append(f"[{cur}]ass={ass_name}[vsub]")
        cur = "vsub"
    tail = "" if win and layers is not None else _out_scale(plan, draft)
    if win:  # 出口で0からにし、足りないフレームは最後を伸ばす(-frames:v で長さを決める)
        tail = "setpts=PTS-STARTPTS,tpad=stop_mode=clone:stop=-1," + tail
    g.append(f"[{cur}]{tail}format=yuv420p[vout]")

    # 4) BGM + 差し込み音声 + 効果音のミックス
    aout = None
    if want_audio:
        aout = "acat"
        bgm = None if "bgm" in plan.mute else d["bgm"]
        mix = []
        if bgm:
            inputs += ["-stream_loop", -1, "-i", proj.resolve(bgm["file"])]
            vol = bgm.get("volume_db", -23.5)
            fade = min(2.0, plan.total / 4)
            g.append(f"[{nin}:a]atrim=0:{plan.total:.4f},asetpts=PTS-STARTPTS,volume={vol}dB,"
                     f"afade=t=out:st={plan.total - fade:.3f}:d={fade:.3f}[bgm]")
            if bgm.get("duck"):
                g.append("[acat]asplit=2[vo][sc]")
                g.append("[bgm][sc]sidechaincompress=threshold=0.03:ratio=8:attack=20:release=400[bgmd]")
                mix = ["[vo]", "[bgmd]"]
            else:
                mix = ["[acat]", "[bgm]"]
        if extra_audio:
            mix = (mix or ["[acat]"]) + extra_audio
        if mix:
            g.append(f"{''.join(mix)}amix=inputs={len(mix)}:duration=first:normalize=0,alimiter=limit=0.841:attack=5:release=50:level=0[amix]")  # 最後に -1.5dB で頭打ち(効果音の重なりで割れないように。AAC で +0.1〜0.5dB はみ出すので -1dBTP に余裕を持たせる)
            aout = "amix"
    return inputs, ";\n".join(g), "vout", aout, subs


def _out_scale(plan, draft):
    """書き出し解像度への縮小フィルタ(末尾に , 付き。縮めないなら空)"""
    oh = plan.proj.data["output"].get("height")  # 書き出し解像度(縦px)。None=元のまま
    return "scale=640:-2," if draft else (f"scale=-2:{int(oh)}:flags=lanczos," if oh and int(oh) != plan.H else "")


def draft_codec_args(o):
    """低画質の試し書き出し。エンコーダーが CPU 指定でなく NVENC があれば GPU で"""
    if o.get("encoder", "auto") != "cpu" and ff.has_encoder("h264_nvenc"):
        return ["-c:v", "h264_nvenc", "-preset", "p1", "-cq", 32, "-b:v", 0]
    return ["-c:v", "libx264", "-preset", "ultrafast", "-crf", 30]


# 画質プリセット → (x264/x265 の CRF, NVENC の CQ)
QUALITY = {"high": (18, 19), "standard": (21, 23), "small": (26, 28)}


def video_codec_args(o):
    """output 設定から映像エンコード引数を作る。codec: h264|h265 / encoder: auto|gpu|cpu / quality: high|standard|small / bitrate_mbps: 指定時は固定ビットレート寄り"""
    h265 = o.get("codec") == "h265"
    enc = o.get("encoder", "auto")
    crf, cq = QUALITY.get(o.get("quality", "high"), QUALITY["high"])
    gpu_name = "hevc_nvenc" if h265 else "h264_nvenc"
    use_gpu = enc == "gpu" or (enc == "auto" and ff.has_encoder(gpu_name))
    br = o.get("bitrate_mbps")
    if use_gpu:
        a = ["-c:v", gpu_name, "-preset", "p5"]
        a += ["-b:v", f"{br}M", "-maxrate", f"{float(br) * 1.5}M", "-bufsize", f"{float(br) * 2}M"] if br else ["-cq", cq, "-b:v", 0]
    else:
        a = ["-c:v", "libx265" if h265 else "libx264", "-preset", "medium"]
        a += ["-b:v", f"{br}M", "-maxrate", f"{float(br) * 1.5}M", "-bufsize", f"{float(br) * 2}M"] if br else ["-crf", crf]
    if h265:
        a += ["-tag:v", "hvc1"]
    return a


def render(proj, out=None, draft=False, rng=None, frame=None, on_progress=None, full=False):
    """書き出し。frame=秒(カット後) なら静止画PNG(full=True で元の解像度)、rng=(a,b) ならその範囲だけ。"""
    if frame is None and rng is None and (proj.cache_dir / "audio.wav").exists():  # 全編は分割書き出し
        out = out or str(proj.resolve(proj.data["output"]["file"]))
        if draft:
            out = str(Path(out).with_name(Path(out).stem + "_draft.mp4"))
        return render_chunked(proj, out, draft=draft, on_progress=on_progress)
    plan = Plan(proj)
    plan.draft = bool(draft and frame is None)
    cache = proj.cache_dir
    inputs, graph, vl, al, subs = build(plan, want_audio=frame is None, draft=draft or (frame is not None and not full), at=frame)
    (cache / "graph.txt").write_text(graph, encoding="utf-8")
    if subs:
        (cache / "subs.ass").write_text(subs, encoding="utf-8")
    args = list(inputs) + ["-filter_complex_script", "graph.txt", "-map", f"[{vl}]"]
    if frame is not None:
        out = out or str(cache / f"frame_{frame:.2f}.png")
        args += ["-ss", f"{frame:.3f}", "-frames:v", 1]
    else:
        out = out or str(proj.resolve(proj.data["output"]["file"]))
        if draft:
            out = str(Path(out).with_name(Path(out).stem + "_draft.mp4"))  # .mov 等でも本番を上書きしない
        args += ["-map", f"[{al}]"]
        if rng:
            args += ["-ss", f"{rng[0]:.3f}", "-t", f"{rng[1] - rng[0]:.3f}"]
        else:
            args += ["-t", f"{plan.total:.3f}"]
        if draft:
            args += draft_codec_args(proj.data["output"])
        else:
            args += video_codec_args(proj.data["output"])
        args += ["-c:a", "aac", "-b:a", _aac_kbps(proj), "-movflags", "+faststart"]
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    ff.run([*args, str(Path(out).resolve())], cwd=cache, on_progress=on_progress,
           total=(rng[1] - rng[0]) if rng else plan.total)
    return {"output": str(Path(out).resolve()), "duration": round((rng[1] - rng[0]) if rng else plan.total, 2), "segments": len(plan.keeps)}


def frames(proj, ts, outdir, width=None):
    """カット後の時刻 ts(昇順)のコマを outdir に PNG で出す。分割書き出しと同じグラフ(win)を1回だけ動かして必要なコマだけ取る
    (1コマずつ ffmpeg を起動するより速い)。時刻はコマの格子に丸める。戻り値 [(実際の時刻, PNG)]"""
    plan = Plan(proj)
    fps = plan.fps
    ts = [min(max(0.0, t), max(0.0, plan.total - 1 / fps)) for t in ts]
    ks = [round(t * fps) for t in ts]  # コマ番号を1回だけ丸める(A と差を別々に丸めると半コマの時刻で食い違い、-1 コマになる)
    A = min(ks) / fps
    ns = sorted({k - min(ks) for k in ks})
    B = min(plan.total, A + (ns[-1] + 1) / fps)
    inputs, graph, vl, _, subs = build(plan, want_audio=False, win=(A, B), gpu=GPU_DEC)
    if subs:
        (proj.cache_dir / "subs.ass").write_text(subs, encoding="utf-8")
    sel = "+".join(rf"eq(n\,{n})" for n in ns)
    graph += f";[{vl}]trim=end_frame={ns[-1] + 1},select='{sel}'" + (f",scale={int(width)}:-2" if width else "") + "[pvf]"
    (proj.cache_dir / "frames_graph.txt").write_text(graph, encoding="utf-8")
    outdir = Path(outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    for old in outdir.glob("f*.png"):
        old.unlink()
    ff.run([*map(str, inputs), "-filter_complex_script", "frames_graph.txt", "-map", "[pvf]", "-fps_mode", "passthrough",
            "-frames:v", str(len(ns)), str((outdir / "f%04d.png").resolve())], cwd=proj.cache_dir)
    return [(round(A + n / fps, 4), outdir / f"f{k + 1:04d}.png") for k, n in enumerate(ns)]
