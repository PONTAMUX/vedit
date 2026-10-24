"""プロジェクトファイル(JSON)の読み書き・検証。"""
import json
import os
import shutil
from pathlib import Path

from .timeutil import parse_time

DEFAULT_STYLES = {
    # サイズ・幅は 1080p 基準のpx。実際の解像度に合わせて自動で拡大縮小される。
    "telop": {
        # 書体・色・フチは 新テロ２.psd 準拠(HGP創英角ゴシックUB・白文字・黒の外側フチ。PSDは103pxに10px → 64pxでは約6px)
        # サイズ・位置は従来どおり(64px / 下から70px)
        "font": "HGP創英角ｺﾞｼｯｸUB", "size": 64, "bold": False, "color": "#FFFFFF",
        # 外側→内側の順。2本以上で二重縁(例: 外=濃紺 / 内=明るい青)
        "outlines": [{"color": "#000000", "width": 6}],
        "align": "bottom", "margin_v": 70,
    },
    "topic": {
        # 既存のトピックバー画像生成(create_topic_bars.py)の x1.2 と同じ見た目:
        # ダークグレー背景 + 左に赤アクセント + 白文字(36px×1.2=43px / アクセント幅 8×1.2 / 余白 16×1.2, 10×1.2)
        "font": "HGP創英角ｺﾞｼｯｸUB", "size": 43, "bold": False, "color": "#FFFFFF",
        "bar_color": "#3C3C3C", "bar_alpha": 1.0, "accent_color": "#C82828", "accent_w": 10,
        "pad_x": 19, "pad_y": 12, "x": 0, "y": 40, "min_width": 0,
    },
}

DEFAULT_AUDIO = {
    # 元素材の音声に先に掛ける処理(Audition相当)。上から順に実行。
    #   {"type":"vst3","path":"...vst3","params":{...}}   VST3プラグイン
    #   {"type":"plugin","name":"NoiseGate","params":{}}  内蔵(Pedalboard)エフェクト
    #   {"type":"declick","params":{"threshold":8}}    自前De-click(先頭に置く。大きいほど控えめ)
    #   {"type":"gain","db":18}
    #   {"type":"level","params":{"g":15,"m":6}}         声の大きさのばらつきをならす(dynaudnorm。先頭寄りに置く)
    #   {"type":"normalize","peak_db":-3}                 ピーク基準。必ず最後に置く
    #   {"type":"normalize","lufs":-14,"peak_db":-1}      ラウドネス基準(peak_db は上限)
    "chain": [
        {"type": "declick", "params": {"threshold": 8}},  # 自前De-click。RX等を使うなら {"type":"vst3","path":...} に差し替え
        {"type": "gain", "db": 18},
        {"type": "normalize", "peak_db": -3},
    ],
}

TEMPLATE = {
    "version": 1,
    "source": "",
    "timebase": "source",  # telops/topics/overlays の時刻が「元素材」基準か "output"(カット後)基準か
    "audio": DEFAULT_AUDIO,
    "splits": [],     # 分割点(元素材の時刻)。編集画面の「分割(E/Ctrl+K)」。書き出しには影響しない
    "cuts": [],       # 削除する範囲 [[開始, 終了], ...]
    "gaps": [],       # 空白(黒+無音) [[元素材の時刻, 長さ秒], ...]。その時刻の素材の直前に挿入。編集画面の「削除(空白を残す)」「クリップ移動」で作られる
    "vmute": [],      # 映像だけ黒にする範囲(元素材の時刻) [[開始,終了],...]。リンク解除して V1 だけ削除
    "amute": [],      # 音声だけ無音にする範囲(元素材の時刻)。リンク解除して A1 だけ削除
    "aslip": [],      # 音声だけ移動 [{"a","b","d"}]: 元素材 a〜b の音を、本来の位置(カット後)から d 秒ずらして鳴らす。元の場所と移動先の元の音は無音
    "telops": [],     # {"start","end","text","style"?}
    "topics": [],     # {"start","end","text"}  左上のトピックバー
    "overlays": [],   # {"file","start","end","pos"?,"scale"?,"in"?,"audio"?}
    "sfx": [],        # 効果音 {"file","start","end"?,"gain_db"?,"in"?}  時刻の基準・カットの扱いは overlays と同じ。end 省略=音の長さ全部
    "cut_fx": {"audio_fade_ms": 8, "video": "none", "dur": 0.12},  # カットのつなぎ目。video: none|dip-black|dip-white
    "bgm": None,      # {"file","volume_db":-23.5,"duck":false}
    "styles": {},
    "output": {"file": "out/final.mp4", "fps": None},
}


def path_fields(d):
    """プロジェクト内のファイルパスの在りか [(入れ物, キー)]。書き出し先(output.file)は含めない(新しいプロジェクトの隣に出す)。"""
    out = [(d, "source")] if d.get("source") else []
    out += [(o, "file") for k in ("overlays", "sfx") for o in d.get(k) or [] if o.get("file")]
    out += [(t["se"], "file") for t in d.get("transitions") or [] if (t.get("se") or {}).get("file")]
    if d.get("bgm") and d["bgm"].get("file"):
        out.append((d["bgm"], "file"))
    out += [(st, "path") for st in (d.get("audio") or {}).get("chain") or [] if st.get("type") == "vst3" and st.get("path")]
    return out


def rebase(p, old_dir, new_dir):
    """old_dir からの相対パス p を、new_dir から同じ実ファイルを指す形に(同じドライブなら相対、違うドライブなら絶対)。"""
    if Path(p).is_absolute():
        return p
    real = Path(os.path.normpath(Path(old_dir) / p))
    try:
        return Path(os.path.relpath(real, new_dir)).as_posix()
    except ValueError:  # ドライブが違うと相対にできない
        return real.as_posix()


class Project:
    def __init__(self, path):
        self.path = Path(path).resolve()
        self.dir = self.path.parent
        with open(self.path, encoding="utf-8") as f:
            self.data = json.load(f)
        for k, v in TEMPLATE.items():
            self.data.setdefault(k, json.loads(json.dumps(v)))

    @classmethod
    def create(cls, path, source):
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        d = json.loads(json.dumps(TEMPLATE))
        d["source"] = str(source)
        path.write_text(json.dumps(d, ensure_ascii=False, indent=2), encoding="utf-8")
        return cls(path)

    def save(self):
        tmp = self.path.with_suffix(".tmp")
        tmp.write_text(json.dumps(self.data, ensure_ascii=False, indent=2), encoding="utf-8")
        os.replace(tmp, self.path)

    def resolve(self, p):
        p = Path(p)
        return p if p.is_absolute() else (self.dir / p)

    def save_as(self, dst, data=None):
        """別名で保存。dst に data(省略時は self.data)を書き、新しい Project を返す。元のファイルはそのまま。
        別フォルダなら相対パスを新しい場所から同じ実ファイルを指すように書き換え、処理済み音声のキャッシュを引き継ぐ。"""
        dst = Path(dst).resolve()
        d = json.loads(json.dumps(self.data if data is None else data))
        if dst.parent != self.dir:
            for it, k in path_fields(d):
                it[k] = rebase(it[k], self.dir, dst.parent)
        dst.parent.mkdir(parents=True, exist_ok=True)
        tmp = dst.with_suffix(".tmp")
        tmp.write_text(json.dumps(d, ensure_ascii=False, indent=2), encoding="utf-8")
        os.replace(tmp, dst)
        new = Project(dst)
        new._take_audio_cache(self)
        return new

    def _take_audio_cache(self, old):
        """元のキャッシュの処理済み音声(audio.wav / audio.json)を、こちらに無ければコピーする(音声処理のやり直しを避ける)。"""
        src_dir, dst_dir = old.dir / (old.path.stem + ".cache"), self.cache_dir
        wav, meta = src_dir / "audio.wav", src_dir / "audio.json"
        if not (wav.is_file() and meta.is_file()) or (dst_dir / "audio.wav").exists():
            return
        info = json.loads(meta.read_text("utf-8"))
        try:  # キーに入っている素材のパスを、こちらのプロジェクトから見た書き方にそろえる(同じ実ファイルの時だけ)
            k = json.loads(info["key"])
            src = self.resolve(self.data["source"])
            if Path(k[0]).resolve() == src.resolve():
                k[0] = str(src)
                info["key"] = json.dumps(k, sort_keys=True)
        except Exception:
            pass
        shutil.copy2(wav, dst_dir / "audio.wav")
        info["file"] = str(dst_dir / "audio.wav")
        (dst_dir / "audio.json").write_text(json.dumps(info, ensure_ascii=False), encoding="utf-8")

    @property
    def cache_dir(self):
        d = self.dir / (self.path.stem + ".cache")
        d.mkdir(exist_ok=True)
        return d

    def style(self, name):
        base = DEFAULT_STYLES.get(name) or DEFAULT_STYLES["telop"]
        return {**base, **self.data["styles"].get(name, {})}

    def validate(self, duration=None):
        """問題点のリスト(文字列)を返す。空なら問題なし。"""
        d, errs = self.data, []
        if not d["source"] or not self.resolve(d["source"]).exists():
            errs.append(f"source が見つかりません: {d['source']!r}")
        if d["timebase"] not in ("source", "output"):
            errs.append("timebase は 'source' か 'output'")

        def rng(kind, i, it):
            try:
                a, b = parse_time(it["start"]), parse_time(it["end"])
            except Exception as e:
                errs.append(f"{kind}[{i}]: start/end が不正 ({e})")
                return
            if b <= a:
                errs.append(f"{kind}[{i}]: end が start 以前です")
            if duration and kind != "cuts" and d["timebase"] == "source" and a > duration:
                errs.append(f"{kind}[{i}]: start が素材の長さを超えています")

        for i, c in enumerate(d["cuts"]):
            rng("cuts", i, {"start": c[0], "end": c[1]})
        for kind in ("telops", "topics", "overlays"):
            for i, it in enumerate(d[kind]):
                if it.get("preset"):  # プリセット(書き出し時に展開)。中身は presets.validate で見る
                    if it.get("start") is not None and it.get("end") is not None:
                        rng(kind, i, it)
                    continue
                rng(kind, i, it)
                if kind == "overlays" and it.get("file") is None and it.get("kind"):
                    continue  # 生成要素(集中線など)。中身は fx.validate で見る
                if kind == "overlays" and not self.resolve(it.get("file", "")).is_file():
                    errs.append(f"overlays[{i}]: ファイルがありません: {it.get('file')!r}")
        for i, it in enumerate(d.get("sfx") or []):
            try:
                a = parse_time(it["start"])
                if it.get("end") is not None and parse_time(it["end"]) <= a:
                    errs.append(f"sfx[{i}]: end が start 以前です(省略すると音の長さ全部)")
                if duration and d["timebase"] == "source" and a > duration:
                    errs.append(f"sfx[{i}]: start が素材の長さを超えています")
                if not (isinstance(it.get("gain_db"), str) and it["gain_db"].startswith("match:")):  # match:基準 は presets.validate
                    float(it.get("gain_db", 0))
                parse_time(it.get("in", 0))
            except Exception as e:
                errs.append(f"sfx[{i}]: start/end/gain_db/in が不正 ({e})")
            if not self.resolve(it.get("file", "")).is_file():
                errs.append(f"sfx[{i}]: ファイルがありません: {it.get('file')!r}")
        for i, g in enumerate(d.get("gaps", [])):
            if len(g) != 2 or float(g[1]) <= 0:
                errs.append(f"gaps[{i}]: [時刻, 長さ>0] にしてください")
        for k in ("vmute", "amute"):
            for i, c in enumerate(d.get(k, [])):
                rng(k, i, {"start": c[0], "end": c[1]})
        for i, s in enumerate(d.get("aslip", [])):
            if not {"a", "b", "d"} <= set(s) or float(s["b"]) <= float(s["a"]):
                errs.append(f"aslip[{i}]: {{a,b,d}} で b > a にしてください")
        from . import fx
        errs += fx.validate(d, self)
        for i, t in enumerate(d.get("transitions") or []):
            se = t.get("se") or {}
            if se.get("file") and not self.resolve(se["file"]).is_file():
                errs.append(f"transitions[{i}].se: ファイルがありません: {se['file']!r}")
        bgm = d["bgm"]
        if bgm and not self.resolve(bgm.get("file", "")).is_file():
            errs.append(f"bgm: ファイルがありません: {bgm.get('file')!r}")
        chain = d["audio"]["chain"]
        for i, st in enumerate(chain):
            if st.get("type") == "normalize" and i != len(chain) - 1:
                errs.append("audio.chain: normalize は最後に置いてください")
            if st.get("type") == "vst3" and not self.resolve(st.get("path", "")).exists():
                errs.append(f"audio.chain[{i}]: VST3 が見つかりません: {st.get('path')!r}")
        return errs
