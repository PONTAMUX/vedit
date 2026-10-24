"""vedit コマンド。出力はすべてJSON(AIエージェントが読みやすいように)。"""
import argparse
import json
import re
import sys
from pathlib import Path

from . import audio, ff, render as rd
from .project import Project, TEMPLATE
from .timeutil import TimeMap, fmt_time, parse_time

SCHEMA_DOC = Path(__file__).resolve().parent.parent / "AGENTS.md"


def _nl(text):
    return text.replace("\\n", "\n")  # コマンドラインの「\n」を改行に


def out(obj):
    print(json.dumps(obj, ensure_ascii=False, indent=2))


def load(a):
    return Project(a.project)


def cmd_init(a):
    src = Path(a.source).resolve()
    p = Project.create(a.project, src)
    info = ff.probe(src)
    out({"project": str(p.path), "source": str(src), **{k: info[k] for k in ("duration", "width", "height")}})


def cmd_info(a):
    p = load(a)
    d = p.data
    plan = rd.Plan(p)
    out({"source": str(plan.src), "source_duration": fmt_time(plan.info["duration"]),
         "output_duration": fmt_time(plan.total), "size": f"{plan.W}x{plan.H}@{plan.fps}",
         "cuts": len(d["cuts"]), "telops": len(d["telops"]), "topics": len(d["topics"]),
         "overlays": len(d["overlays"]), "sfx": len(d.get("sfx") or []), "bgm": bool(d["bgm"]),
         "audio_chain": [s["type"] for s in d["audio"]["chain"]]})


def cmd_validate(a):
    p = load(a)
    dur = ff.probe(p.resolve(p.data["source"]))["duration"] if p.resolve(p.data["source"]).exists() else None
    errs = p.validate(dur)
    out({"ok": not errs, "errors": errs})
    sys.exit(1 if errs else 0)


def cmd_check(a):
    from . import check
    p = load(a)
    res = check.run(p)
    if a.output:
        m, more = check.verify_output(p, a.output, rd.Plan(p).total)
        res += more
        out({"ok": not res, "audio": m, "checks": res})
    else:
        out({"ok": not res, "checks": res})
    sys.exit(1 if any(r["level"] == "warn" for r in res) else 0)


def cmd_audio(a):
    out(audio.process(load(a), force=a.force))


def cmd_silence(a):
    p = load(a)
    audio.process(p)  # ノーマライズ後の音で判定する(生の収録音は小さいため)
    found = audio.detect_silence(p, a.min, a.noise)
    cuts = [[round(s + a.keep_before, 3), round(e - a.keep_after, 3)] for s, e in found]
    cuts = [c for c in cuts if c[1] - c[0] > 0.05]
    if a.apply:
        p.data["cuts"] = sorted(p.data["cuts"] + cuts)
        p.save()
    tm = TimeMap(ff.probe(p.resolve(p.data["source"]))["duration"], p.data["cuts"] + ([] if a.apply else cuts))
    out({"applied": a.apply, "new_cuts": len(cuts), "removed_sec": round(sum(b - a_ for a_, b in cuts), 2),
         "result_duration": fmt_time(tm.total), "sample": cuts[:5]})


def _add(a, kind, item):
    p = load(a)
    p.data[kind].append(item)
    p.data[kind].sort(key=lambda it: parse_time(it["start"] if isinstance(it, dict) else it[0]))
    p.save()
    out({"added": item, "count": len(p.data[kind])})


def cmd_cut(a):
    _add(a, "cuts", [parse_time(a.start), parse_time(a.end)])


def cmd_telop(a):
    it = {"start": parse_time(a.start), "end": parse_time(a.end), "text": _nl(a.text)}
    if a.style:
        it["style"] = a.style
    _add(a, "telops", it)


def cmd_topic(a):
    _add(a, "topics", {"start": parse_time(a.start), "end": parse_time(a.end), "text": _nl(a.text)})


def cmd_overlay(a):
    it = {"file": a.file, "start": parse_time(a.start), "end": parse_time(a.end)}
    if a.pos:
        it["pos"] = a.pos
    if a.scale:
        it["scale"] = a.scale
    if a.audio:
        it["audio"] = True
    if a.skip:
        it["in"] = parse_time(a.skip)
    _add(a, "overlays", it)


def cmd_sfx(a):
    """効果音を置く(時刻は overlays と同じ基準)。開始はカンマ区切りで複数、ファイルもカンマ区切りなら交互に置く。"""
    p = load(a)
    files = a.file.split(",")
    for f in files:
        if not p.resolve(f).is_file():
            raise SystemExit(f"ファイルがありません: {f}")
    starts = a.start.split(",")
    if a.end and len(starts) > 1:
        raise SystemExit("--end は開始が1つの時だけ使えます")
    added = []
    for n, t in enumerate(starts):
        it = {"file": files[n % len(files)], "start": parse_time(t), "gain_db": a.gain if str(a.gain).startswith("match:") else float(a.gain)}
        if a.end:
            it["end"] = parse_time(a.end)
        if a.skip:
            it["in"] = parse_time(a.skip)
        p.data["sfx"].append(it)
        added.append(it)
    p.data["sfx"].sort(key=lambda it: parse_time(it["start"]))
    p.save()
    out({"added": added, "count": len(p.data["sfx"])})


def cmd_srt(a):
    """SRT を telops として取り込む(時刻は元素材基準)。"""
    p = load(a)
    text = Path(a.file).read_text(encoding="utf-8-sig")
    items = []
    for blk in re.split(r"\r?\n\r?\n", text.strip()):
        m = re.search(r"(\d+):(\d+):(\d+)[,.](\d+)\s*-->\s*(\d+):(\d+):(\d+)[,.](\d+)\s*\r?\n(.+)", blk, re.S)
        if m:
            g = m.groups()
            s = int(g[0]) * 3600 + int(g[1]) * 60 + int(g[2]) + int(g[3]) / 1000
            e = int(g[4]) * 3600 + int(g[5]) * 60 + int(g[6]) + int(g[7]) / 1000
            items.append({"start": round(s, 3), "end": round(e, 3), "text": g[8].strip()})
    p.data["telops"] = (p.data["telops"] if a.append else []) + items
    p.save()
    out({"imported": len(items)})


def cmd_render(a):
    p = load(a)
    errs = p.validate()
    if errs:
        out({"ok": False, "errors": errs})
        sys.exit(1)
    audio.process(p)
    rng = (parse_time(a.range[0]), parse_time(a.range[1])) if a.range else None
    out(rd.render(p, out=a.out, draft=a.draft, rng=rng))


def cmd_frame(a):
    p = load(a)
    out(rd.render(p, out=a.out, frame=parse_time(a.time)))


def contact_sheet(tiles, out, cols=5, width=384):
    """[(見出し, PNG のパス)] → 並べた1枚の PNG"""
    from PIL import Image, ImageDraw
    ims = []
    for label, f in tiles:
        im = Image.open(f).convert("RGB")
        im = im.resize((width, round(im.height * width / im.width)))
        ImageDraw.Draw(im).text((6, 4), label, fill=(255, 255, 0), stroke_width=2, stroke_fill=(0, 0, 0))
        ims.append(im)
    cols = min(cols, len(ims))
    rows = -(-len(ims) // cols)
    th = max(i.height for i in ims)
    sheet = Image.new("RGB", (cols * width + (cols - 1) * 4, rows * th + (rows - 1) * 4), (40, 40, 40))
    for i, im in enumerate(ims):
        sheet.paste(im, ((i % cols) * (width + 4), (i // cols) * (th + 4)))
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    sheet.save(out)
    return str(Path(out).resolve())


def cmd_frames(a):
    """t0〜t1 を step 秒おきに静止画にして並べる(動きの確認用)"""
    import tempfile
    p = load(a)
    t0, t1, step = parse_time(a.start), parse_time(a.end), float(a.step)
    ts, t = [], t0
    while t <= t1 + 1e-6 and len(ts) < 200:
        ts.append(round(t, 4))
        t += step
    with tempfile.TemporaryDirectory() as tmp:
        got = rd.frames(p, ts, tmp, width=640)
        out({"output": contact_sheet([(f"{t:.2f}", f) for t, f in got], a.out, cols=a.cols), "frames": len(got)})


def cmd_fx(a):
    """fx list: 使えるエフェクト等の一覧(JSON) / fx preview <名前>: 見本の動きを数コマ並べた画像"""
    import tempfile
    from . import fx
    if a.action == "list":
        if a.md:  # AGENTS.md の一覧表
            return print(fx.catalog_md())
        return out(fx.catalog())
    if a.action != "preview" or not a.name:
        raise SystemExit("使い方: vedit fx list / vedit fx preview <名前> [--params JSON] -o sheet.png")
    try:
        user = json.loads(a.params) if a.params else {}
    except json.JSONDecodeError as e:
        raise SystemExit(f"--params が JSON ではありません: {e}")
    proj, ts, what = fx.sample_project(a.name, user, frames=a.frames)
    errs = proj.validate()
    if errs:
        out({"ok": False, "errors": errs})
        sys.exit(1)
    o = a.out or str(proj.dir / f"{what.replace(':', '_')}.png")
    with tempfile.TemporaryDirectory() as tmp:
        got = rd.frames(proj, ts, tmp, width=480)
        out({"name": what, "output": contact_sheet([(f"{t - ts[0]:.2f}s", f) for t, f in got], o, cols=a.cols, width=480), "frames": len(got)})


def cmd_review(a):
    """修正対応: start=今の状態を修正前として控える / next=次の回(今を修正前として控え直し、前の回は残す) / add=修正点を足す(「0:07 画像」の行。時刻は修正前の動画の時刻) / list / done N"""
    from . import review
    p = load(a)
    r = a.round - 1 if a.round else None  # 修正点N(1 始まり)。省略で最新の回
    if a.action == "next":
        rd = review.next_round(p, video=a.video)
        return out({"review": str(review.path_of(p)), "round": len(review.rounds(review.load(p))), "video": rd["video"]})
    if a.action == "start":
        rv = review.start(p, video=a.video, force=a.force)
        return out({"review": str(review.path_of(p)), "video": rv["video"]})
    if a.action == "add":
        text = Path(a.file).read_text("utf-8") if a.file else "\n".join(a.args)
        n = review.add(p, text, r=r)
        return out({"added": n, "review": str(review.path_of(p))})
    if a.action == "export":  # vreview の管理ページに貼る文章(1行1点、時刻は今の動画)
        try:
            text = review.export_text(p, undone=a.undone, r=r)
        except ValueError as e:
            raise SystemExit(str(e))
        if a.out:
            Path(a.out).write_text(text, encoding="utf-8")
        if a.clip:
            import subprocess
            subprocess.run(["clip"], input=b"\xff\xfe" + text.encode("utf-16-le"), check=True, creationflags=ff.NOWIN)
        if not a.out and not a.clip:
            sys.stdout.buffer.write(text.encode("utf-8"))
        else:
            out({"points": text.count("\n"), "output": str(Path(a.out).resolve()) if a.out else None, "clip": bool(a.clip)})
        return
    if a.action in ("done", "undone"):
        for i in a.args:
            review.update(p, int(i), r=r, done=a.action == "done")
    st = review.state(p, r)
    if not st["exists"]:
        return out(st)
    tc = review.timemap(p, p.data)
    out({"round": st["round"] + 1, "rounds": len(st["rounds"]), "video": st["video"], "points": [{"i": x["i"], "t": fmt_time(x["t"]), "now": fmt_time(tc.out(x["src"])), "src": x["src"],
                                           "text": x["text"], "done": x["done"]} for x in st["points"]],
         "changes": [{**c, "now": fmt_time(tc.out(c["src"])) if c["src"] is not None else None} for c in st["changes"]]})


def cmd_expand(a):
    """プリセットを展開し、match の音量を数値にした JSON(書き出しで実際に使う形)を出す。元のファイルは書き換えない"""
    from . import presets
    p = load(a)
    errs = p.validate()
    if errs:
        out({"ok": False, "errors": errs})
        sys.exit(1)
    d = presets.expand_data(p.data, p)
    if a.out:
        Path(a.out).write_text(json.dumps(d, ensure_ascii=False, indent=2), encoding="utf-8")
        out({"ok": True, "output": str(Path(a.out).resolve())})
    else:
        out(d)


def cmd_gui(a):
    from . import server
    target = Path(a.project) if a.project else None
    if target is None or target.suffix.lower() != ".json":
        if target is None:  # 引数なしで起動したらファイル選択
            picked = server.pick_file("video")
            if not picked:
                raise SystemExit("動画が選ばれませんでした")
            target = Path(picked)
        proj = target.with_suffix(".vedit.json")  # 動画を渡されたら隣にプロジェクトを作る
        if not proj.exists():
            Project.create(proj, target.name)
        target = proj
    server.serve(target.resolve(), open_window=not a.no_window, port=a.port)


def cmd_import_keys(a):
    """Premiere のショートカット(.kys)を取り込む。省略時はこのPCの最新のカスタム.kysを探す。"""
    from . import keys
    kys = Path(a.project) if a.project else keys.find_kys()
    if not kys or not Path(kys).exists():
        raise SystemExit(".kys が見つかりません。パスを指定してください")
    km = keys.convert(kys)
    dst = keys.user_path()
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(json.dumps(km, ensure_ascii=False, indent=2), encoding="utf-8")
    out({"from": str(kys), "saved": str(dst), "keymap": km})


def cmd_image(a):
    """画像編集の文書(*.vimg.json)を透過PNGに書き出す(描画は GUI と同じページを Edge headless で)。"""
    from . import vimg
    out(vimg.export(a.project, a.out, timeout=a.timeout))


def cmd_vimg(a):
    """画像編集アプリ vimg(ブイイメージ)を単独で開く(動画プロジェクトなし)。"""
    from . import server
    server.serve_vimg(a.project, open_window=not a.no_window, port=a.port, idle=a.idle)


def cmd_schema(a):
    print(SCHEMA_DOC.read_text(encoding="utf-8") if SCHEMA_DOC.exists() else json.dumps(TEMPLATE, ensure_ascii=False, indent=2))


def main(argv=None):
    ap = argparse.ArgumentParser(prog="vedit", description="動画編集特化ツール(音声処理→カット→テロップ→差し込み→トピックバー)")
    sub = ap.add_subparsers(dest="cmd", required=True)

    def add(name, fn, help, *args):
        s = sub.add_parser(name, help=help)
        s.add_argument("project", nargs="?" if name in ("schema", "gui", "import-keys", "vimg") else None, help="project.json のパス")
        for spec, kw in args:
            s.add_argument(*spec if isinstance(spec, tuple) else (spec,), **kw)
        s.set_defaults(fn=fn)

    add("init", cmd_init, "元動画からプロジェクトを新規作成", ("source", {}))
    add("info", cmd_info, "プロジェクトの要約")
    add("validate", cmd_validate, "プロジェクトの検証(書き出し前チェック)")
    add("check", cmd_check, "仕上がりの見落としチェック(画像の効果音抜け・カットで消える効果音・同じトラックの重なり・音声処理の古さ)",
        (("--output",), {"help": "書き出したファイルの音量/ピーク/長さも検査"}))
    add("audio", cmd_audio, "音声処理(VST3→ゲイン→ノーマライズ)を実行", (("--force",), {"action": "store_true"}))
    add("silence", cmd_silence, "無音区間を検出してカット候補にする",
        (("--min",), {"type": float, "default": 0.7}), (("--noise",), {"type": float, "default": -40}),
        (("--keep-before",), {"type": float, "default": 0.2}), (("--keep-after",), {"type": float, "default": 0.35}),
        (("--apply",), {"action": "store_true"}))
    add("cut", cmd_cut, "削除範囲を追加", ("start", {}), ("end", {}))
    add("telop", cmd_telop, "テロップを追加", ("start", {}), ("end", {}), ("text", {}), (("--style",), {}))
    add("topic", cmd_topic, "トピックバーを追加", ("start", {}), ("end", {}), ("text", {}))
    add("overlay", cmd_overlay, "映像/画像の差し込みを追加", ("file", {}), ("start", {}), ("end", {}),
        (("--pos",), {"help": "full|center|top-left|top-right|bottom-left|bottom-right"}),
        (("--scale",), {"type": float, "help": "画面幅に対する比率(posがfull以外)"}),
        (("--skip",), {"help": "差し込み動画の開始位置(秒)"}), (("--audio",), {"action": "store_true"}))
    add("sfx", cmd_sfx, "効果音を置く(開始をカンマ区切りで複数・ファイルもカンマ区切りなら交互に置く)", ("file", {}), ("start", {}),
        (("--gain",), {"default": 0.0, "help": "音量(dB)。match:ぽペン で基準の音とピークをそろえる(match:ぽペン-3 のように増減も)"}),
        (("--end",), {"help": "ここで切る(省略=音の長さ全部。開始が1つの時だけ)"}), (("--skip",), {"help": "効果音の頭から飛ばす秒"}))
    add("import-srt", cmd_srt, "SRTをテロップとして取り込む", ("file", {}), (("--append",), {"action": "store_true"}))
    add("render", cmd_render, "書き出し", (("-o", "--out"), {}), (("--draft",), {"action": "store_true", "help": "低画質で速く"}),
        (("--range",), {"nargs": 2, "metavar": ("START", "END"), "help": "カット後の時刻の範囲だけ"}))
    add("frame", cmd_frame, "指定時刻(カット後)の静止画PNGを出力して仕上がり確認", ("time", {}), (("-o", "--out"), {}))
    add("frames", cmd_frames, "start〜end(カット後)を step 秒おきに静止画にして1枚に並べる(動きの確認)", ("start", {}), ("end", {}),
        (("--step",), {"default": "0.1"}), (("--cols",), {"type": int, "default": 5}), (("-o", "--out"), {"required": True}))
    fxp = sub.add_parser("fx", help="エフェクト・動き・トランジション・文字アニメーター・プリセットの一覧(list)と見本の画像(preview)")
    fxp.add_argument("action", choices=["list", "preview"])
    fxp.add_argument("name", nargs="?", help="preview する名前(同じ名前が複数の種類にある時は anim:pop / text:pop / preset:gloom のように種類を付ける)")
    fxp.add_argument("--params", help='パラメータの上書き(JSON。例: {"amp":[20,10]})')
    fxp.add_argument("--frames", type=int, default=10, help="並べるコマ数")
    fxp.add_argument("--cols", type=int, default=5)
    fxp.add_argument("--md", action="store_true", help="list を AGENTS.md の表(Markdown)で出す")
    fxp.add_argument("-o", "--out")
    fxp.set_defaults(fn=cmd_fx)
    add("expand", cmd_expand, "プリセットを展開した JSON を見る(書き出しで使う形。元のファイルは変えない)",
        (("-o", "--out"), {"help": "書き出すファイル(省略で画面に出す)"}))
    add("review", cmd_review, "修正対応: start(今を修正前として控える)/ add \"0:07 画像\" …(修正点。時刻は修正前の動画)/ list / done N"
        " / export(vreview に貼る文章。時刻は今の動画)",
        ("action", {"choices": ["start", "next", "add", "list", "done", "undone", "export"]}), ("args", {"nargs": "*"}),
        (("--out",), {"help": "export: 書き出すファイル(UTF-8)"}), (("--undone",), {"action": "store_true", "help": "export: 未済の点だけ"}),
        (("--clip",), {"action": "store_true", "help": "export: クリップボードへ"}),
        (("--file",), {"help": "add: 修正点を書いたテキスト"}), (("--video",), {"help": "start: 修正前の動画(省略で output.file か同名の mp4)"}),
        (("--force",), {"action": "store_true", "help": "start: 控えを作り直す(修正点は消える)"}),
        (("--round",), {"type": int, "help": "修正点N(1 始まり。省略で最新の回)。next で次の回を足す"}))
    add("gui", cmd_gui, "編集画面(GUI)を開く", (("--no-window",), {"action": "store_true"}), (("--port",), {"type": int, "default": 0}))
    add("import-keys", cmd_import_keys, "Premiere のショートカット(.kys)を取り込む(省略時は自動検出)")
    add("image", cmd_image, "画像編集の文書(*.vimg.json)を透過PNGに書き出す", (("-o", "--out"), {"help": "省略時は同名の .png"}),
        (("--timeout",), {"type": float, "default": 90}))
    add("vimg", cmd_vimg, "画像編集アプリ vimg を単独で開く(引数: .vimg.json / 画像 / フォルダ。省略で新規・開く画面)",
        (("--no-window",), {"action": "store_true"}), (("--port",), {"type": int, "default": 0}),
        (("--idle",), {"type": float, "default": 180, "help": "窓を閉じてから自動終了するまでの秒数(0=終了しない)"}))
    add("schema", cmd_schema, "プロジェクト形式と使い方(AGENTS.md)を表示")

    a = ap.parse_args(argv)
    a.fn(a)


if __name__ == "__main__":
    main()
