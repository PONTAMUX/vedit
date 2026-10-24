"""ffmpeg / ffprobe の探索と実行。"""
import json
import os
import shutil
import subprocess
import sys

# GUI(pythonw・コンソールなし)から ffmpeg などのコンソールアプリを起動すると、そのたびに黒い窓が一瞬出る。必ずこれを付ける。
# GUI の時は優先度も「通常以下」にして、裏の処理(試聴用音声の作り直し等)でプレビュー再生がかくつかないようにする。
NOWIN = getattr(subprocess, "CREATE_NO_WINDOW", 0) | (getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0) if sys.stdout is None else 0)


def find(name):
    env = os.environ.get("VEDIT_" + name.upper())
    if env and os.path.isfile(env):
        return env
    p = shutil.which(name)
    if p:
        return p
    raise SystemExit(f"{name} が見つかりません。PATH に通すか、環境変数 VEDIT_{name.upper()} に exe のパスを設定してください。")


_probe_cache = None
# 調べた結果はディスクにも残す(Premiere の .ims と同じ考え方)。起動・書き出しのたびに数十本を ffprobe し直すと数秒かかるため
_PROBE_FILE = os.path.join(os.environ.get("LOCALAPPDATA") or os.path.expanduser("~"), "vedit", "probe_cache.json")


def _load_probe_cache():
    global _probe_cache
    if _probe_cache is None:
        try:
            with open(_PROBE_FILE, encoding="utf-8") as f:
                _probe_cache = json.load(f)
        except Exception:
            _probe_cache = {}
    return _probe_cache


def _save_probe_cache():
    try:
        os.makedirs(os.path.dirname(_PROBE_FILE), exist_ok=True)
        tmp = f"{_PROBE_FILE}.{os.getpid()}.tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(_probe_cache, f, ensure_ascii=False)
        os.replace(tmp, _PROBE_FILE)
    except Exception:
        pass


def probe(path):
    """ffprobe の要約。同じファイル(更新時刻・サイズが同じ)なら結果を使い回す(ディスクにも保存)。"""
    try:
        st = os.stat(path)
        key = f"{os.path.abspath(path)}|{st.st_mtime_ns}|{st.st_size}"
    except OSError:
        key = None
    cache = _load_probe_cache()
    if key in cache:
        return dict(cache[key])
    info = _probe(path)
    if key:
        cache[key] = dict(info)
        if len(cache) > 20000:  # 古いものから捨てる(dict は入れた順)
            for k in list(cache)[:5000]:
                del cache[k]
        _save_probe_cache()
    return info


def _probe(path):
    out = subprocess.run(
        [find("ffprobe"), "-v", "error", "-print_format", "json", "-show_format", "-show_streams", str(path)],
        capture_output=True, text=True, encoding="utf-8", check=True, creationflags=NOWIN).stdout
    j = json.loads(out)
    v = next((s for s in j["streams"] if s["codec_type"] == "video"), None)
    a = next((s for s in j["streams"] if s["codec_type"] == "audio"), None)
    info = {"duration": float(j["format"]["duration"]), "has_audio": a is not None}
    if v:
        n, d = (v.get("r_frame_rate") or "30/1").split("/")
        info.update(width=int(v["width"]), height=int(v["height"]), fps=float(n) / float(d or 1))
    return info


def run(args, cwd=None, on_progress=None, total=None):
    """ffmpeg 実行。on_progress(0..1) を渡すと進捗を通知する(GUI用)。"""
    base = [find("ffmpeg"), "-hide_banner", "-y", "-loglevel", "error"]
    if on_progress is None:
        r = subprocess.run([*base, "-stats", *map(str, args)], cwd=cwd, creationflags=0 if sys.stdout else NOWIN)
        code = r.returncode
    else:
        p = subprocess.Popen([*base, "-progress", "pipe:1", "-nostats", *map(str, args)], cwd=cwd,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace",
                             creationflags=NOWIN)
        for line in p.stdout:
            if line.startswith("out_time_us=") and total:
                try:
                    on_progress(min(1.0, int(line.split("=")[1]) / 1e6 / total))
                except ValueError:
                    pass
        err = p.stderr.read()
        code = p.wait()
        if code:
            raise RuntimeError(f"ffmpeg が失敗しました: {err[-600:]}")
    if code:
        raise SystemExit(f"ffmpeg が失敗しました (code {code})")


_enc_cache = {}


def has_encoder(name):
    if name not in _enc_cache:  # GUI(pythonw)から呼んでもコンソール窓を出さない
        out = subprocess.run([find("ffmpeg"), "-hide_banner", "-encoders"], capture_output=True, text=True,
                             creationflags=NOWIN).stdout
        _enc_cache[name] = f" {name} " in out
    return _enc_cache[name]


_cuda = None


def has_cuda():
    """NVIDIA の GPU で重ね合わせ(hwupload → overlay_cuda → hwdownload)が動くか。小さい絵で1回だけ試して覚えておく。"""
    global _cuda
    if _cuda is None:
        try:
            r = subprocess.run([find("ffmpeg"), "-hide_banner", "-loglevel", "error", "-init_hw_device", "cuda=gpu", "-filter_hw_device", "gpu",
                                "-f", "lavfi", "-i", "color=black:s=64x64:d=0.1", "-f", "lavfi", "-i", "color=red@0.5:s=16x16:d=0.1",
                                "-filter_complex", "[0:v]format=yuv420p,hwupload[a];[1:v]format=yuva420p,hwupload[b];"
                                "[a][b]overlay_cuda=x=8:y=8,hwdownload,format=yuv420p", "-f", "null", "-"],
                               capture_output=True, timeout=30, creationflags=NOWIN)
            _cuda = r.returncode == 0
        except Exception:
            _cuda = False
    return _cuda


def vram_free_mib():
    """NVIDIA の GPU の空き VRAM(MiB)。調べられなければ None。"""
    try:
        out = subprocess.run(["nvidia-smi", "--query-gpu=memory.free", "--format=csv,noheader,nounits"], capture_output=True, text=True,
                             timeout=10, creationflags=NOWIN).stdout
        return int(out.split()[0])
    except Exception:
        return None
