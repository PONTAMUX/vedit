"""音声処理(Audition相当): VST3 / 内蔵エフェクト → ゲイン → ピークノーマライズ。"""
import json
import math
import re
import subprocess

from . import ff
from .declick import declick

CHUNK = 48000 * 10


def _build_plugin(project, st):
    import pedalboard as pb
    t = st["type"]
    if t == "gain":
        return pb.Gain(gain_db=float(st["db"]))
    if t == "vst3":
        pl = pb.load_plugin(str(project.resolve(st["path"])))
    elif t == "plugin":
        pl = getattr(pb, st["name"])()
    else:
        raise SystemExit(f"不明な audio ステップ: {t}")
    for k, v in st.get("params", {}).items():
        setattr(pl, k, v)
    return pl


_BOARD = None


def _pool_init(proj_path, specs, pre):
    """並列処理の子プロセス: プラグインを1回だけ読み込んで持っておく。"""
    global _BOARD
    from .project import Project
    proj = Project(proj_path)
    _BOARD = _make_board(proj, specs, pre)


def _make_board(project, specs, pre=0.0):
    import pedalboard as pb
    return pb.Pedalboard(([pb.Gain(gain_db=pre)] if pre else []) + [_build_plugin(project, s) for s in specs])


def _pool_work(args):
    from pedalboard.io import AudioFile
    path, lo, hi = args
    with AudioFile(path) as f:
        f.seek(lo)
        return _BOARD(f.read(hi - lo), f.samplerate, reset=True)


def _workers(project, n_chunks):
    """VST3(RX など1コアで重いもの)があるときだけ別プロセスで並列に回す。書き出しと同じ CPU 上限(output.cpu_percent)に合わせる"""
    if not any(s["type"] == "vst3" for s in project.data["audio"]["chain"]):
        return 1
    from .render import _cpu_cores
    return max(1, min(n_chunks, 8, _cpu_cores(project)[1] - 1))


def process(project, force=False):
    """元素材の音声を処理して <proj>.cache/audio.wav に書く。結果の情報dictを返す。"""
    src = project.resolve(project.data["source"])
    cache = project.cache_dir
    out, meta = cache / "audio.wav", cache / "audio.json"
    chain = project.data["audio"]["chain"]
    key = json.dumps([str(src), src.stat().st_mtime, chain], sort_keys=True)
    if not force and out.exists() and meta.exists():
        old = json.loads(meta.read_text("utf-8"))
        if old.get("key") == key:
            return old

    import numpy as np
    import pedalboard as pb
    from pedalboard.io import AudioFile

    raw, stage = cache / "audio_raw.wav", cache / "audio_stage.wav"
    norm = next((s for s in chain if s["type"] == "normalize"), None)
    clickers = [s for s in chain if s["type"] == "declick"]
    plugins = [s for s in chain if s["type"] not in ("normalize", "declick", "level")]  # 読み込みは _make_board で(並列時は子プロセス側)
    # 重い段(最後の VST3 まで。RX など)の結果は audio_heavy.f32 に残す。後ろの軽い段(コンプ・リミッター・目標 LUFS)
    # だけを変えたときは、元音声の取り出しも RX も飛ばして数秒で終わる(Kdenlive/Olive の段ごとのキャッシュと同じ考え。
    # 区間ごとに reset=True で通すので「重い段→軽い段」に分けても結果は1本で通したときとビット単位で同じ。2026-10-11 確認)
    last = max((i for i, s in enumerate(plugins) if s["type"] == "vst3"), default=-1)
    heavy, light = plugins[:last + 1], plugins[last + 1:]
    hfile, hmeta = cache / "audio_heavy.f32", cache / "audio_heavy.json"
    hkey = json.dumps([str(src), src.stat().st_mtime, [s for s in chain if s["type"] in ("declick", "level")], heavy], sort_keys=True)
    pre_of = lambda lr: max(-30.0, min(40.0, float(norm["lufs"]) - lr)) if lr is not None and norm and norm.get("lufs") is not None and plugins else 0.0
    hit = None
    if heavy and not force and hfile.exists() and hmeta.exists():
        try:
            hit = json.loads(hmeta.read_text("utf-8"))
            if hit.get("key") != hkey or hit.get("pre") != pre_of(hit.get("lufs_raw")) or hfile.stat().st_size != hit.get("bytes"):
                hit = None
        except Exception:
            hit = None
    if hit:
        clicks, lufs_raw, pre = hit["clicks"], hit["lufs_raw"], hit["pre"]
        sr, nch, frames = hit["sr"], hit["ch"], hit["frames"]
    else:
        hmeta.unlink(missing_ok=True)
        hfile.unlink(missing_ok=True)
        ff.run(["-i", src, "-vn", "-c:a", "pcm_f32le", "-ar", 48000, raw])
        clicks = 0
        if clickers:  # 自前De-click(先頭で実行)。区間の継ぎ目で途切れないよう前後に余白を付けて処理
            clean = cache / "audio_declick.wav"
            with AudioFile(str(raw)) as fin, AudioFile(str(clean), "w", fin.samplerate, fin.num_channels, bit_depth=32) as fout:
                step, pad = fin.samplerate * 20, 2048
                for a in range(0, fin.frames, step):
                    lo, hi = max(0, a - pad), min(fin.frames, a + step + pad)
                    fin.seek(lo)
                    seg = fin.read(hi - lo)
                    for st in clickers:
                        seg, n = declick(seg, fin.samplerate, **st.get("params", {}))
                        clicks += n
                    fout.write(seg[:, a - lo: a - lo + min(step, fin.frames - a)])
            raw.unlink()
            clean.replace(raw)
        for st in [s for s in chain if s["type"] == "level"]:  # 声の大きさのばらつきをならす(ffmpeg dynaudnorm。数秒単位で上げ下げし、語ごとには動かさない)
            q = {"f": 500, "g": 15, "p": 0.7, "m": 6, "t": 0.003, **st.get("params", {})}
            lev = cache / "audio_level.wav"
            ff.run(["-i", raw, "-af", "dynaudnorm=" + ":".join(f"{k}={v}" for k, v in q.items()), "-c:a", "pcm_f32le", lev])
            raw.unlink()
            lev.replace(raw)
        lufs_raw = None
        if norm and norm.get("lufs") is not None and plugins:
            # ラウドネス基準のときは先に目標付近まで上げ下げしてからエフェクトに通す
            # (ゲート/コンプのしきい値が録音の大小に左右されない。小さい録音で声がゲートに消されるのを防ぐ)
            lufs_raw = loudness(raw)
        pre = pre_of(lufs_raw)
        with AudioFile(str(raw)) as fin:
            sr, nch, frames = fin.samplerate, fin.num_channels, fin.frames
    peak = 0.0
    # 1回ずつ reset=True で通す(pedalboard がプラグインの遅れを補正する。reset=False の連続処理だと
    # RX De-click などの遅れぶん声が後ろへずれる)。前後に2秒の余白を付けて処理し、余白は捨てる(コンプ等の状態をつなぐ)
    # 継ぎ目は 50ms のクロスフェード(区間ごとの処理結果がわずかに違い、そのままだと段差でプチッと鳴る)
    # 区間ごとに独立しているので、RX などの重い VST3 は別プロセスで並列に回す(結果は1本で回したときと同じ。
    # スレッド並列は RX の結果が崩れたので使わない。2026-10-11 実作業: 3並列で 2.2倍速)
    step, pad, xf = sr * 30, sr * 2, sr // 20
    ramp, tail = np.linspace(0.0, 1.0, xf, dtype=np.float32), None
    starts = list(range(0, frames, step))
    spans = [(max(0, a - pad), min(frames, a + step + xf + pad)) for a in starts]
    ex, hout = None, None
    if hit:  # 重い段は残しておいた結果を読む
        offs = [0]
        for lo, hi in spans:
            offs.append(offs[-1] + nch * (hi - lo))

        def heavy_res():
            for i, (lo, hi) in enumerate(spans):
                yield np.fromfile(hfile, np.float32, offs[i + 1] - offs[i], offset=offs[i] * 4).reshape(nch, hi - lo)
        results = heavy_res()
    else:
        nw = _workers(project, len(starts))
        first = heavy if heavy else plugins
        if nw > 1:
            from concurrent.futures import ProcessPoolExecutor
            ex = ProcessPoolExecutor(nw, initializer=_pool_init, initargs=(str(project.path), first, pre))
            results = ex.map(_pool_work, [(str(raw), lo, hi) for lo, hi in spans])
        else:
            board = _make_board(project, first, pre)

            def seq():
                with AudioFile(str(raw)) as fin:
                    for lo, hi in spans:
                        fin.seek(lo)
                        yield board(fin.read(hi - lo), fin.samplerate, reset=True)
            results = seq()
        if heavy:
            hout = open(cache / "audio_heavy.f32.tmp", "wb")
    lboard = _make_board(project, light) if heavy and light else None
    with AudioFile(str(stage), "w", sr, nch, bit_depth=32) as fout:
        for a, (lo, hi), res in zip(starts, spans, results):
            if hout:
                np.ascontiguousarray(res, np.float32).tofile(hout)
            if lboard is not None:
                res = lboard(res, sr, reset=True)
            res = res[:, a - lo: a - lo + min(step + xf, frames - a)]
            if tail is not None:
                n = min(xf, res.shape[1], tail.shape[1])
                res[:, :n] = tail[:, :n] * (1 - ramp[:n]) + res[:, :n] * ramp[:n]
            tail, res = res[:, step:], res[:, :step]
            peak = max(peak, float(np.abs(res).max()))
            fout.write(res)
    if ex:
        ex.shutdown()
    if hout:
        hout.close()
        (cache / "audio_heavy.f32.tmp").replace(hfile)
        hmeta.write_text(json.dumps({"key": hkey, "lufs_raw": lufs_raw, "pre": pre, "clicks": clicks, "sr": sr, "ch": nch,
                                     "frames": frames, "bytes": hfile.stat().st_size}), encoding="utf-8")
    peak_db = 20 * math.log10(peak) if peak > 0 else -120.0
    gain, lufs_before = 0.0, None
    if norm and norm.get("lufs") is not None:  # ラウドネス基準(上限ピークを超えないよう抑える)
        lufs_before = loudness(stage)
        gain = min(float(norm["lufs"]) - lufs_before, float(norm.get("peak_db", -1)) - peak_db)
    elif norm:
        gain = float(norm["peak_db"]) - peak_db
    ff.run(["-i", stage, "-af", f"volume={gain:.4f}dB", "-c:a", "pcm_s24le", out])
    raw.unlink(missing_ok=True)
    stage.unlink(missing_ok=True)
    info = {"key": key, "peak_before_norm_db": round(peak_db, 2), "norm_gain_db": round(gain + pre, 2), "pre_gain_db": round(pre, 2), "lufs_before": round(lufs_raw, 1) if lufs_raw is not None else lufs_before and round(lufs_before, 1),
            "lufs_after": lufs_before and round(lufs_before + gain, 1), "clicks_repaired": clicks, "file": str(out)}
    meta.write_text(json.dumps(info, ensure_ascii=False), encoding="utf-8")
    return info


def loudness(path):
    """統合ラウドネス(LUFS)を測る。"""
    r = subprocess.run([ff.find("ffmpeg"), "-hide_banner", "-nostats", "-i", str(path), "-vn", "-af", "ebur128", "-f", "null", "-"],
                       capture_output=True, text=True, encoding="utf-8", errors="replace", creationflags=ff.NOWIN)
    m = re.findall(r"I:\s+(-?[\d.]+) LUFS", r.stderr)
    if not m:
        raise SystemExit("ラウドネスを測れませんでした")
    return float(m[-1])


def detect_silence(project, min_len=0.7, noise_db=-40):
    """無音区間 [(start,end)] を返す。処理済み音声があればそれを使う。"""
    wav = project.cache_dir / "audio.wav"
    src = wav if wav.exists() else project.resolve(project.data["source"])
    r = subprocess.run([ff.find("ffmpeg"), "-hide_banner", "-i", str(src), "-vn", "-af",
                        f"silencedetect=noise={noise_db}dB:d={min_len}", "-f", "null", "-"],
                       capture_output=True, text=True, encoding="utf-8", errors="replace", creationflags=ff.NOWIN)
    starts = [float(x) for x in re.findall(r"silence_start: (-?[\d.]+)", r.stderr)]
    ends = [float(x) for x in re.findall(r"silence_end: ([\d.]+)", r.stderr)]
    if len(starts) > len(ends):
        ends.append(ff.probe(src)["duration"])
    return [(max(0.0, s), e) for s, e in zip(starts, ends)]


_DECODED = {}  # (パス, 更新時刻, sr, ch) → 波形。効果音は数種類を使い回すので、試聴用を作り直すたびに ffmpeg で読み直さない(Premiere の .cfa と同じ考え)


def _decode(path, sr, ch):
    """音声ファイルを float32 の (フレーム, ch) に読む(ffmpeg で sr / ch にそろえる)。読んだものはメモリに残す(合計 256MB まで)。"""
    import os
    st = os.stat(path)
    key = (str(path), st.st_mtime_ns, st.st_size, sr, ch)
    if key not in _DECODED:
        while _DECODED and sum(v.nbytes for v in _DECODED.values()) > 256 << 20:
            _DECODED.pop(next(iter(_DECODED)))
        _DECODED[key] = _decode_raw(path, sr, ch)
        _DECODED[key].flags.writeable = False
    return _DECODED[key]


def _decode_raw(path, sr, ch):
    import numpy as np
    r = subprocess.run([ff.find("ffmpeg"), "-hide_banner", "-loglevel", "error", "-i", str(path), "-vn",
                        "-f", "f32le", "-ac", str(ch), "-ar", str(sr), "-"], capture_output=True, creationflags=ff.NOWIN)
    if r.returncode:
        raise SystemExit(f"効果音を読めません: {path}")
    return np.frombuffer(r.stdout, "<f4").reshape(-1, ch)


def preview_cut(project, pieces, sfx=None):
    """GUI の試聴用に、処理済み音声をカット後の並びにつないだ 16bit WAV を作る(書き出しと同じく継ぎ目は短くフェード)。
    pieces: 出力順の [{"s","e"}(元素材の秒) | {"gap": 秒}]。同じ並びなら作り直さない。キャッシュ内の古い試聴用は消す。
    sfx: 効果音 [{"file","at"(出力上の秒),"len"(秒),"in"?,"gain_db"?}]。書き出しと同じく最後に足し込む。"""
    import hashlib
    import wave
    import numpy as np
    src = project.cache_dir / "audio.wav"
    if not src.exists():
        raise SystemExit("処理済み音声がありません")
    fade_ms = float((project.data.get("cut_fx") or {}).get("audio_fade_ms", 8))
    sfx = [x for x in (sfx or []) if float(x.get("len", 0)) > 0]
    stamps = {x["file"]: project.resolve(x["file"]).stat().st_mtime for x in sfx}  # 差し替えたら作り直す
    key = hashlib.sha1(json.dumps([src.stat().st_mtime, fade_ms, pieces, sfx, stamps], sort_keys=True).encode()).hexdigest()[:12]
    out = project.cache_dir / f"preview_{key}.wav"
    if out.exists():
        return out
    for old in project.cache_dir.glob("preview_*.wav"):
        try:
            old.unlink()
        except OSError:  # 再生中で開かれている
            pass
    tmp = out.with_suffix(".tmp")
    with wave.open(str(src), "rb") as fi:
        ch, sw, sr, n = fi.getnchannels(), fi.getsampwidth(), fi.getframerate(), fi.getnframes()
        if sw not in (2, 3, 4):
            raise SystemExit(f"未対応のWAV形式です(サンプル幅 {sw})")
        nf = max(1, int(sr * fade_ms / 1000))
        ramp = np.linspace(0, 1, nf, dtype=np.float32)[:, None]
        # 効果音: (出力上の開始フレーム, 波形×音量)。同じファイルは1回だけ読む
        decoded, fx = {}, []
        for x in sfx:
            if x["file"] not in decoded:
                decoded[x["file"]] = _decode(project.resolve(x["file"]), sr, ch)
            d = decoded[x["file"]]
            s0 = max(0, int(round(float(x.get("in", 0)) * sr)))
            seg = d[s0:s0 + int(round(float(x["len"]) * sr))] * np.float32(10 ** (float(x.get("gain_db", 0)) / 20))
            if len(seg):
                fx.append((int(round(float(x["at"]) * sr)), seg))
        pos = 0  # 書き込んだフレーム数(= 出力上の位置)

        def put(fo, x):
            nonlocal pos
            for p0, seg in fx:  # 重なる所だけ float にして足す(長い区間を丸ごと変換しない)
                a, b = max(pos, p0), min(pos + len(x), p0 + len(seg))
                if a < b:
                    y = x[a - pos:b - pos].astype(np.float32) + seg[a - p0:b - p0] * 32767
                    x[a - pos:b - pos] = np.clip(np.rint(y), -32768, 32767)
            fo.writeframes(x.tobytes())
            pos += len(x)

        with wave.open(str(tmp), "wb") as fo:
            fo.setnchannels(ch)
            fo.setsampwidth(2)
            fo.setframerate(sr)
            for p in pieces:
                if "gap" in p:
                    put(fo, np.zeros((int(round(float(p["gap"]) * sr)), ch), "<i2"))
                    continue
                a, b = max(0, int(round(float(p["s"]) * sr))), min(n, int(round(float(p["e"]) * sr)))
                if b <= a:
                    continue
                fi.setpos(a)
                raw = np.frombuffer(fi.readframes(b - a), np.uint8).reshape(-1, sw)
                x = raw[:, sw - 2:].copy().view("<i2").reshape(-1, ch)  # 上位16bit
                if len(x) > 2 * nf:  # 継ぎ目のフェード(書き出しの audio_fade_ms と同じ)
                    x = x.astype(np.float32)
                    x[:nf] *= ramp
                    x[-nf:] *= ramp[::-1]
                    x = x.astype("<i2")
                put(fo, x)  # 本編の後ろにはみ出した効果音は書き出しと同じく切れる
    tmp.replace(out)
    return out
