"""GUI 用のローカルサーバー(標準ライブラリのみ)。127.0.0.1 限定 + 起動ごとのトークンで保護。"""
import base64
import gzip
import hashlib
import json
import mimetypes
import os
import secrets
import shutil
import subprocess
import sys
import threading
import time
import traceback
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

from . import audio, ff, fx, proxy, render as rd, review, sysmon, vimg
from .project import DEFAULT_STYLES, Project
from .timeutil import parse_time

WEB = Path(__file__).resolve().parent / "web"
JOBS = {}
PREVIEW_LOCK = threading.Lock()


# ---------------- ジョブ(音声処理・書き出しなど時間のかかるもの) ----------------
def start_job(fn):
    jid = uuid.uuid4().hex[:8]
    job = JOBS[jid] = {"status": "running", "progress": 0.0, "result": None, "error": None}

    def work():
        try:
            job["result"] = fn(lambda p: job.__setitem__("progress", p))
            job["progress"], job["status"] = 1.0, "done"
        except BaseException as e:  # SystemExit も拾う
            job["error"], job["status"] = f"{e}" or traceback.format_exc(), "error"

    threading.Thread(target=work, daemon=True).start()
    return jid


def job_audio(proj, force=False):
    return lambda prog: audio.process(proj, force=force)


def job_silence(proj, a):
    def run(prog):
        audio.process(proj)
        found = audio.detect_silence(proj, float(a.get("min", 0.7)), float(a.get("noise", -40)))
        kb, ka = float(a.get("keep_before", 0.2)), float(a.get("keep_after", 0.35))
        cuts = [[round(s + kb, 3), round(e - ka, 3)] for s, e in found]
        return {"cuts": [c for c in cuts if c[1] - c[0] > 0.05]}
    return run


def job_render(proj, a):
    def run(prog):
        errs = proj.validate()
        if errs:
            raise RuntimeError("; ".join(errs))
        audio.process(proj)
        rng = (parse_time(a["range"][0]), parse_time(a["range"][1])) if a.get("range") else None
        return rd.render(proj, draft=bool(a.get("draft")), rng=rng, on_progress=prog)
    return run


def job_frame(proj, a):
    return lambda prog: rd.render(proj, frame=float(a["t"]))


# ---------------- Photoshop 連携 ----------------
def find_photoshop():
    cands = []
    for base in (os.environ.get("ProgramFiles"), os.environ.get("ProgramFiles(x86)")):
        if base:
            cands += list(Path(base, "Adobe").glob("Adobe Photoshop*/Photoshop.exe"))
    return str(max(cands, key=lambda p: p.stat().st_mtime)) if cands else None


def photoshop(proj, a):
    """Photoshop で開く。action: open(既存ファイル) / frame(今の画面を原寸PNGに) / new(透明PNGを新規作成)。開いたパスを返す。"""
    exe = find_photoshop()
    if not exe:
        raise RuntimeError("Photoshop が見つかりません")
    act = a.get("action", "open")
    if act == "launch":
        subprocess.Popen([exe], creationflags=NORMAL)
        return None
    folder = proj.dir / "ps"
    folder.mkdir(exist_ok=True)
    if act == "open":
        path = proj.resolve(a["path"])
    elif act == "frame":
        t = float(a["t"])
        path = folder / f"frame_{int(t // 60):02d}m{t % 60:05.2f}s.png"
        rd.render(proj, out=str(path), frame=t, full=True)
    elif act == "new":
        info = ff.probe(proj.resolve(proj.data["source"]))
        w, h = info.get("width") or 1920, info.get("height") or 1080
        n = 1
        while (folder / f"image_{n:02d}.png").exists():
            n += 1
        path = folder / f"image_{n:02d}.png"
        ff.run(["-f", "lavfi", "-i", f"color=c=black@0.0:s={w}x{h},format=rgba", "-frames:v", 1, path])
    else:
        raise RuntimeError(f"不明な action: {act}")
    subprocess.Popen([exe, str(path)], creationflags=NORMAL)
    return str(path)


# ---------------- 補助 ----------------
def list_fonts():
    names = set()
    try:
        import winreg
        for root in (winreg.HKEY_LOCAL_MACHINE, winreg.HKEY_CURRENT_USER):
            try:
                with winreg.OpenKey(root, r"SOFTWARE\Microsoft\Windows NT\CurrentVersion\Fonts") as k:
                    for i in range(winreg.QueryInfoKey(k)[1]):
                        # 「HG創英角ｺﾞｼｯｸUB & HGP創英角ｺﾞｼｯｸUB & HGS創英角ｺﾞｼｯｸUB (TrueType)」のように TTC は & 区切り
                        for n in winreg.EnumValue(k, i)[0].split(" (")[0].split(" & "):
                            for suf in (" Bold", " Italic", " Regular", " Light", " Semibold", " Semilight", " Medium", " Black", " Heavy"):
                                n = n.replace(suf, "")
                            names.add(n.strip())
            except OSError:
                pass
    except ImportError:
        pass
    if not names:  # レジストリが読めない環境はファイル名で代用
        d = Path(os.environ.get("WINDIR", "C:/Windows")) / "Fonts"
        names = {f.stem for f in d.glob("*.tt[fc]")} if d.is_dir() else set()
    return sorted(n for n in names if n)


def waveform(proj, per_sec=50, src=None):
    """波形(1秒 per_sec 個のピーク 0〜255)。src 省略で本編の音声、指定でその素材ファイル(効果音・BGM)。"""
    import hashlib
    import numpy as np
    if src is None:
        wav = proj.cache_dir / "audio.wav"
        src = wav if wav.exists() else proj.resolve(proj.data["source"])
        cache = proj.cache_dir / "wave.json"
    else:
        cache = proj.cache_dir / f"wave_{hashlib.md5(str(src.resolve()).encode()).hexdigest()[:12]}.json"
    key = f"{src}:{src.stat().st_mtime}:{per_sec}"
    if cache.exists():
        c = json.loads(cache.read_text("utf-8"))
        if c.get("key") == key:
            return c
    raw = subprocess.run([ff.find("ffmpeg"), "-v", "error", "-i", str(src), "-vn", "-ac", "1", "-ar", "8000",
                          "-f", "s16le", "-"], capture_output=True, creationflags=ff.NOWIN).stdout
    x = np.abs(np.frombuffer(raw, dtype=np.int16).astype(np.float32)) / 32768.0
    step = 8000 // per_sec
    x = x[: len(x) // step * step].reshape(-1, step).max(axis=1)
    out = {"key": key, "rate": per_sec, "peaks": [int(round(v * 255)) for v in x]}
    cache.write_text(json.dumps(out), encoding="utf-8")
    return out


def pick_save(name, initial_dir):
    """名前を付けて保存ダイアログ(書き出し先の指定用)。選んだパス or None。"""
    name = Path(name or "final.mp4").name.replace("'", "")
    ps = ("Add-Type -AssemblyName System.Windows.Forms;$d=New-Object System.Windows.Forms.SaveFileDialog;"
          "$d.Filter='MP4 (*.mp4)|*.mp4|MOV (*.mov)|*.mov';$d.Title='書き出し先';"
          f"$d.FileName='{name}';$d.InitialDirectory='{str(initial_dir).replace(chr(39), '')}';"
          "if($d.ShowDialog() -eq 'OK'){[Console]::OutputEncoding=[Text.Encoding]::UTF8;Write-Output $d.FileName}")
    r = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True, encoding="utf-8", creationflags=ff.NOWIN)
    return r.stdout.strip() or None


def pick_save_vimg(name, initial_dir):
    """vimg の「名前を付けて保存」ダイアログ。.png を選んでも同名の .vimg.json を返す。"""
    name = Path(name or "image.vimg.json").name.replace("'", "")
    ps = ("Add-Type -AssemblyName System.Windows.Forms;$d=New-Object System.Windows.Forms.SaveFileDialog;"
          "$d.Filter='vimg 文書 + 透過PNG (*.vimg.json)|*.vimg.json|PNG (*.png)|*.png';$d.Title='名前を付けて保存(編集データ .vimg.json と 透過PNG)';"
          f"$d.FileName='{name}';" + (f"$d.InitialDirectory='{str(initial_dir).replace(chr(39), '')}';" if initial_dir else "") +
          "if($d.ShowDialog() -eq 'OK'){[Console]::OutputEncoding=[Text.Encoding]::UTF8;Write-Output $d.FileName}")
    r = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True, encoding="utf-8", creationflags=ff.NOWIN)
    p = r.stdout.strip()
    if not p:
        return None
    if vimg.is_doc(p):  # 文書名で選んだとき、無関係の同名PNG(文書なし)を黙って上書きしないよう連番にする
        d, n = Path(p), 1
        base = d.name[:-len(vimg.EXT)]
        while not d.exists() and vimg.png_of(d).exists():
            n += 1
            d = d.with_name(f"{base}_{n}{vimg.EXT}")
        return d
    s = Path(p)
    stem = s.name[:-len(s.suffix)] if s.suffix.lower() in (".png", ".json") else s.name
    if stem.lower().endswith(".vimg"):
        stem = stem[:-5]
    return s.with_name(stem + vimg.EXT)


def default_saveas_name(path):
    """別名で保存の既定名: <今の名前>_2.vedit.json(あれば _3, _4 …)。"""
    path = Path(path)
    base = path.name[:-len(".vedit.json")] if path.name.lower().endswith(".vedit.json") else path.stem
    n = 2
    while path.with_name(f"{base}_{n}.vedit.json").exists():
        n += 1
    return f"{base}_{n}.vedit.json"


def pick_save_project(path):
    """プロジェクトの「別名で保存」ダイアログ。選んだパス or None。拡張子が無ければ .vedit.json を付ける。"""
    path = Path(path)
    ps = ("Add-Type -AssemblyName System.Windows.Forms;$d=New-Object System.Windows.Forms.SaveFileDialog;"
          "$d.Filter='vedit プロジェクト (*.vedit.json)|*.vedit.json|JSON (*.json)|*.json';$d.Title='プロジェクトを別名で保存';"
          f"$d.FileName='{default_saveas_name(path).replace(chr(39), '')}';$d.InitialDirectory='{str(path.parent).replace(chr(39), '')}';"
          "if($d.ShowDialog() -eq 'OK'){[Console]::OutputEncoding=[Text.Encoding]::UTF8;Write-Output $d.FileName}")
    r = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True, encoding="utf-8", creationflags=ff.NOWIN)
    p = r.stdout.strip()
    if not p:
        return None
    return Path(p if p.lower().endswith(".json") else p + ".vedit.json")


def save_project_as(srv, dst, data):
    """今のプロジェクトを dst に別名で保存し、以後の保存・監視・メディア参照・キャッシュを dst に切り替える。
    dst が今のファイル自身なら普通の保存。新しいプロジェクトのパスを返す。"""
    cur = Project(srv.project_path)
    dst = Path(dst).resolve()
    if dst == cur.path:
        cur.data = data
        cur.save()
        return cur.path
    new = cur.save_as(dst, data)
    srv.project_path = str(new.path)
    return new.path


def pick_file(kind, initial_dir=None):
    """Windows の標準ファイル選択ダイアログ(別プロセス)。選んだパス or None。"""
    types = {"video": "動画 (*.mp4;*.mov;*.mkv;*.avi;*.webm)|*.mp4;*.mov;*.mkv;*.avi;*.webm",
             "media": "画像/動画 (*.png;*.jpg;*.jpeg;*.webp;*.mp4;*.mov;*.mkv;*.webm)|*.png;*.jpg;*.jpeg;*.webp;*.mp4;*.mov;*.mkv;*.webm",
             "audio": "音声 (*.mp3;*.wav;*.m4a;*.aac;*.flac)|*.mp3;*.wav;*.m4a;*.aac;*.flac",
             "image": "画像 (*.png;*.jpg;*.jpeg;*.webp)|*.png;*.jpg;*.jpeg;*.webp",
             "vimg": "vimg 文書・画像 (*.vimg.json;*.png;*.jpg;*.jpeg;*.webp;*.bmp;*.gif)|*.vimg.json;*.png;*.jpg;*.jpeg;*.webp;*.bmp;*.gif",
             "vst3": "VST3 (*.vst3)|*.vst3"}[kind]
    init = f"$d.InitialDirectory='{str(initial_dir).replace(chr(39), '')}';" if initial_dir else ""
    ps = ("Add-Type -AssemblyName System.Windows.Forms;$d=New-Object System.Windows.Forms.OpenFileDialog;"
          f"$d.Filter='{types}|すべて (*.*)|*.*';$d.Title='ファイルを選択';{init}"
          "if($d.ShowDialog() -eq 'OK'){[Console]::OutputEncoding=[Text.Encoding]::UTF8;Write-Output $d.FileName}")
    r = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True, encoding="utf-8", creationflags=ff.NOWIN)
    return r.stdout.strip() or None


def media_ver(p):
    """処理済み音声・プレビュー映像の版(更新時刻)。素材を差し替えて作り直した時に GUI が古いものを使い続けないように"""
    def m(f):
        try:
            return str(f.stat().st_mtime_ns)
        except OSError:
            return ""
    return {"audio_ver": m(p.cache_dir / "audio.wav"), "proxy_ver": m(p.cache_dir / "proxy.mp4")}


def sfx_durations(p):
    """効果音ファイルの長さ(秒) {プロジェクトに書かれたパス: 秒}。GUI で end を省いた効果音の長さに使う。"""
    out = {}
    for it in p.data.get("sfx") or []:
        f = it.get("file")
        if f and f not in out:
            try:
                out[f] = ff.probe(p.resolve(f))["duration"]
            except Exception:
                pass
    return out


# ---------------- HTTP ----------------
class Handler(BaseHTTPRequestHandler):
    server_version = "vedit"
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    # --- 共通 ---
    @property
    def proj(self):
        return Project(self.server.project_path)

    def _ok_auth(self, q):
        host_ok = self.headers.get("Host", "").split(":")[0] in ("127.0.0.1", "localhost")
        tok = self.headers.get("X-Token") or (q.get("t") or [""])[0]
        return host_ok and secrets.compare_digest(tok, self.server.token)

    def _json(self, obj, code=200):
        body = json.dumps(obj, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self.send_response(code)
        if len(body) > 16384 and "gzip" in (self.headers.get("Accept-Encoding") or ""):  # 波形など大きい応答は圧縮
            body = gzip.compress(body, 5)
            self.send_header("Content-Encoding", "gzip")
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return json.loads(self.rfile.read(n) or b"{}")

    @property
    def standalone(self):  # vimg 単独起動(動画プロジェクトなし)
        return getattr(self.server, "vimg_mode", False)

    def _base(self):  # 相対パスの基準フォルダ(GUI=プロジェクトのフォルダ / 画像書き出し=文書のフォルダ / 単独=起動フォルダ)
        if self.standalone:
            return Path(self.server.vimg_dir or Path.cwd())
        return Path(self.server.image_root) if self.server.image_root else self.proj.dir

    def _res(self, p):
        p = Path(p)
        return p if p.is_absolute() else self._base() / p

    def _rel(self, path):
        if self.standalone:  # 単独起動は常に絶対パスでやり取りする
            return Path(path).resolve().as_posix()
        try:
            return Path(path).resolve().relative_to(self._base().resolve()).as_posix()
        except ValueError:
            return str(path)

    def _allowed(self, path):
        path = path.resolve()
        if self.standalone:  # 開いたファイル/選んだフォルダの配下と、開いた文書が参照する画像だけ
            s = self.server
            return path in s.vimg_refs or any(r == path or r in path.parents for r in s.vimg_roots)
        if self.server.image_root:
            return path in self.server.image_refs or Path(self.server.image_root).resolve() in path.parents
        # プロジェクトのフォルダの中(プロキシ・キャッシュ・素材の大半)は、参照の一覧を作らずに通す。
        # 一覧は project.json が変わった時だけ作り直す(素材が千個近いと resolve だけで 100ms かかり、動画のシーク・画像の読み込みのたびに待たされていた)
        srv = self.server
        pp = Path(srv.project_path)
        if pp.resolve().parent in path.parents or path in getattr(srv, "picked", ()):
            return True
        st = pp.stat()
        key = (st.st_mtime_ns, st.st_size)
        cache = getattr(srv, "refs_cache", None)
        if not cache or cache[0] != key:
            p = self.proj
            d = p.data
            refs = [p.resolve(d["source"])] + [p.resolve(o["file"]) for k in ("overlays", "sfx") for o in d.get(k) or [] if o.get("file")]
            refs += [p.resolve(t["se"]["file"]) for t in d.get("transitions") or [] if isinstance(t.get("se"), dict) and t["se"].get("file")]  # トランジションの効果音
            if d["bgm"]:
                refs.append(p.resolve(d["bgm"]["file"]))
            cache = srv.refs_cache = (key, {r.resolve() for r in refs if r})
        return path in cache[1]

    # --- GET ---
    def do_GET(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        try:
            if u.path in ("/", "/index.html"):
                return self._static("image.html" if self.standalone else "index.html")
            if u.path == "/image.html":  # 画像編集(別窓)
                return self._static("image.html")
            if u.path == "/review.html":  # 修正点(別窓)
                return self._static("review.html")
            if u.path.startswith("/static/"):
                return self._static(u.path[len("/static/"):])
            if u.path == "/favicon.ico":  # V-edit のアイコン(web/icon.ico。作り直しは web/make_icon.py)
                return self._static("icon.ico")
            if not self._ok_auth(q):
                return self._json({"error": "unauthorized"}, 403)
            if u.path == "/api/state":
                return self._json(self._state())
            if u.path == "/api/fstat":  # Photoshop で保存されたか(更新時刻)を見る
                f = self.proj.resolve(q["p"][0])
                return self._json({"mtime": f.stat().st_mtime if self._allowed(f) and f.exists() else None})
            if u.path == "/api/sys":  # CPU / GPU / メモリの使用率(右上の表示)
                return self._json(sysmon.stats())
            if u.path == "/api/mtime":
                return self._json({"mtime": self.proj.path.stat().st_mtime, **media_ver(self.proj)})
            if u.path == "/api/review":  # 修正点の一覧・修正前との対応(review.py)。?r=回(0 始まり。省略で最新)
                return self._json(review.state(self.proj, q["r"][0] if q.get("r") else None))
            if u.path == "/api/review/export":  # vreview に貼る文章(text/plain)
                try:
                    body = review.export_text(self.proj, undone=q.get("undone", ["0"])[0] == "1", r=q["r"][0] if q.get("r") else None).encode("utf-8")
                except ValueError as e:
                    return self._json({"error": str(e)}, 400)
                self.send_response(200)
                self.send_header("Content-Type", "text/plain; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                self.wfile.write(body)
                return
            if u.path == "/api/job":
                return self._json(JOBS.get(q["id"][0], {"status": "error", "error": "unknown job"}))
            if u.path == "/api/waveform":
                if q.get("p"):  # 効果音・BGM の素材
                    f = self._res(q["p"][0])
                    if not (self._allowed(f) and f.exists()):
                        return self._json({"error": "not found"}, 404)
                    return self._json({**waveform(self.proj, src=f), "file": True})
                return self._json(waveform(self.proj))
            if u.path == "/api/keymap":
                from . import keys
                return self._json(keys.load())
            if u.path == "/api/fonts":
                return self._json(list_fonts())
            if u.path == "/api/vimg":  # 画像編集の文書を読む
                f = self._res(q["p"][0])
                if not (vimg.is_doc(f) and self._allowed(f)):
                    return self._json({"error": "プロジェクト外のファイルです"}, 403)
                temp = False
                if self.standalone:  # 文書が参照している画像(フォルダ外でも)を見られるように
                    self.server.vimg_refs.update(vimg.refs(f))
                    self.server.vimg_last = f.parent
                    temp = Path(self.server.vimg_tmp).resolve() in f.resolve().parents
                return self._json({"doc": json.loads(f.read_text("utf-8")), "path": self._rel(f), "png": self._rel(vimg.png_of(f)),
                                   "temp": temp})
            if u.path == "/api/vimg/info":  # 画像編集: 単独起動か(メニューの出し分け)・起動フォルダの一覧
                if not self.standalone:
                    return self._json({"standalone": False})
                d = self.server.vimg_dir
                files = []
                if d and Path(d).is_dir():
                    for x in sorted(Path(d).iterdir(), key=lambda x: -x.stat().st_mtime):
                        n = x.name.lower()
                        if x.is_file() and (vimg.is_doc(n) or (x.suffix.lower() in vimg.RASTER and ".src." not in n
                                                               and not x.with_name(x.stem + vimg.EXT).exists())):
                            files.append(x.resolve().as_posix())
                        if len(files) >= 80:
                            break
                return self._json({"standalone": True, "dir": d and Path(d).resolve().as_posix(), "files": files})
            if u.path == "/api/ping":  # 単独起動: 窓が開いている間の生存確認(閉じたら自動終了)
                self.server.vimg_ping = time.time()
                return self._json({"ok": True})
            if u.path == "/media":
                f = self._res(q["p"][0])
                if q.get("pv") and getattr(self, "proj", None) and self._allowed(f):
                    f = preview_video(self.proj, f, q["pv"][0] == "a") if f.suffix.lower() in (".mp4", ".mov", ".mkv", ".webm", ".m4v", ".avi") else preview_image(self.proj, f)
                return self._media(f)
            self._json({"error": "not found"}, 404)
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as e:
            self._json({"error": str(e)}, 500)

    def _state(self):
        p = self.proj
        src = p.resolve(p.data["source"])
        info = ff.probe(src) if src.exists() else None
        meta = p.cache_dir / "audio.json"
        return {"project": p.data, "info": info, "mtime": p.path.stat().st_mtime, "dir": str(p.dir), "name": p.path.stem,
                "audio": json.loads(meta.read_text("utf-8")) if meta.exists() else None,
                "errors": p.validate(info["duration"] if info else None),
                "has_processed": (p.cache_dir / "audio.wav").exists(), "default_styles": DEFAULT_STYLES,
                "sfx_dur": sfx_durations(p), "proxy": str(proxy.ready(p) or "") or None, **media_ver(p)}

    def _static(self, name):
        f = (WEB / name).resolve()
        if WEB.resolve() not in f.parents or not f.is_file():
            return self._json({"error": "not found"}, 404)
        data = f.read_bytes()
        if name.endswith(".html"):
            if self.headers.get("Host", "").split(":")[0] not in ("127.0.0.1", "localhost"):
                return self._json({"error": "forbidden"}, 403)
            data = data.replace(b"__TOKEN__", self.server.token.encode())
        self.send_response(200)
        self.send_header("Content-Type", (mimetypes.guess_type(name)[0] or "text/plain") + "; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def _media(self, path):
        if not path.is_file() or not self._allowed(path):
            return self._json({"error": "forbidden"}, 403)
        size = path.stat().st_size
        start, end, code = 0, size - 1, 200
        rng = self.headers.get("Range")
        if rng and rng.startswith("bytes="):
            a, _, b = rng[6:].partition("-")
            start = int(a) if a else max(0, size - int(b))
            end = int(b) if (b and a) else size - 1
            end, code = min(end, size - 1), 206
        self.send_response(code)
        self.send_header("Content-Type", mimetypes.guess_type(str(path))[0] or "application/octet-stream")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(end - start + 1))
        if code == 206:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        with open(path, "rb") as f:
            f.seek(start)
            left = end - start + 1
            while left > 0:
                chunk = f.read(min(1 << 20, left))
                if not chunk:
                    break
                self.wfile.write(chunk)
                left -= len(chunk)

    # --- POST ---
    def _vimg(self, act, a):
        """画像編集の API。new(新規) / open(既存画像→文書) / save(文書+PNG) / asset(画像の取り込み)。"""
        if act == "new":  # kind: image(透明) / telop(テロップ入り) / frame(今の画面を下敷きに)
            p, kind = self.proj, a.get("kind", "image")
            src = p.resolve(p.data["source"])
            info = ff.probe(src) if src.is_file() else {}
            W, H = info.get("width") or 1920, info.get("height") or 1080
            folder = p.dir / "img"
            path = vimg.unique(folder, "telop" if kind == "telop" else "image")
            layers = []
            if kind == "telop":
                layers.append(vimg.telop_layer(p.style("telop"), W, H))
            elif kind == "frame":  # 原寸の静止画をロックした下敷きに(書き出しには含めない)
                t = float(a["t"])
                bg = folder / f"frame_{int(t // 60):02d}m{t % 60:05.2f}s.png"
                rd.render(p, out=str(bg), frame=t, full=True)
                layers.append({"type": "image", "name": "下敷き(書き出さない)", "src": bg.name, "x": W / 2, "y": H / 2,
                               "w": W, "h": H, "rot": 0, "locked": True, "export": False})
            png = vimg.create(path, vimg.new_doc(W, H, layers))
            return {"path": self._rel(path), "png": self._rel(png)}
        if act == "open":  # .vimg.json はそのまま。画像なら隣の .vimg.json(無ければ作る)
            f = self._res(a["path"])
            created = False
            if not self._allowed(f) or not f.is_file():
                raise PermissionError("プロジェクト外のファイルです")
            if not vimg.is_doc(f):
                if f.suffix.lower() not in vimg.RASTER:
                    raise ValueError("画像ではありません")
                side = f.with_name(f.stem + vimg.EXT)
                created = not side.exists()
                f = side if side.exists() else vimg.from_raster(f)
            return {"path": self._rel(f), "png": self._rel(vimg.png_of(f)), "created": created}
        if act == "save":
            f = self._res(a["path"])
            if self.server.image_root:  # CLI の書き出し: PNG だけを指定の出力先へ
                if a.get("error"):
                    self.server.export_result.update(error=a["error"])
                else:
                    vimg.write_png(self.server.export_out, a["png"])
                    self.server.export_result.update(width=a.get("width"), height=a.get("height"))
                self.server.export_done.set()
                return {"png": str(self.server.export_out)}
            png = vimg.png_of(f)
            if not (vimg.is_doc(f) and self._allowed(f) and self._allowed(png)):
                raise PermissionError("プロジェクト外には保存できません")
            if a.get("doc") is not None:
                vimg.write_json(f, a["doc"])
            if a.get("png"):
                vimg.write_png(png, a["png"])
            return {"png": self._rel(png), "mtime": png.stat().st_mtime if png.exists() else None}
        if act == "asset":  # 画像を文書の隣の assets/ に取り込み、文書からの相対パスを返す
            doc = self._res(a["doc"])
            if not self._allowed(doc):
                raise PermissionError("プロジェクト外のファイルです")
            if a.get("from"):  # ファイル選択で選んだもの。プロジェクト内ならそのまま参照
                src = Path(a["from"])
                if self._allowed(src):
                    try:
                        return {"src": Path(os.path.relpath(src.resolve(), doc.parent.resolve())).as_posix()}
                    except ValueError:  # ドライブが違うと相対にできない
                        return {"src": src.resolve().as_posix()}
                name, data = src.name, src.read_bytes()
            else:  # ドラッグ&ドロップ・貼り付け(base64)
                name, data = Path(a.get("name") or "image.png").name, base64.b64decode(a["data"].split(",")[-1])
            if Path(name).suffix.lower() not in vimg.RASTER:
                raise ValueError("画像ではありません")
            folder = doc.parent / "assets"
            folder.mkdir(exist_ok=True)
            dst, n = folder / name, 1
            while dst.exists():
                n += 1
                dst = folder / f"{Path(name).stem}_{n}{Path(name).suffix}"
            dst.write_bytes(data)
            return {"src": "assets/" + dst.name}
        if act == "launch":  # 動画編集GUIから: 単独の画像編集(vimg)を別プロセス・別窓で起動(プロジェクトに紐づかない新規)
            launch_vimg()
            return {"ok": True}
        if not self.standalone:
            raise ValueError(f"不明な操作: {act}")
        # ---- ここから単独起動(vimg)だけの操作 ----
        s = self.server
        if act == "blank":  # 新規: 保存先が決まるまでは一時フォルダに置く(保存時に「名前を付けて保存」)
            W, H = max(1, min(16384, int(a.get("width") or 1920))), max(1, min(16384, int(a.get("height") or 1080)))
            bg = a.get("background")
            folder = Path(s.vimg_tmp) / uuid.uuid4().hex[:8]
            folder.mkdir(parents=True, exist_ok=True)
            path = folder / ("無題" + vimg.EXT)
            vimg.create(path, vimg.new_doc(W, H, [], bg if isinstance(bg, str) and bg.startswith("#") else None))
            return {"path": self._rel(path)}
        if act == "pick_open":  # ファイルを開くダイアログ。選んだファイルのフォルダを閲覧許可に足す
            picked = pick_file("vimg", a.get("dir") or s.vimg_last or s.vimg_dir)
            if not picked:
                return {"path": None}
            f = Path(picked).resolve()
            s.vimg_roots.add(f.parent)
            return {"path": f.as_posix()}
        if act == "saveas":  # 名前を付けて保存。参照画像は新しい場所から辿れるように src を書き換える
            old = self._res(a["path"]) if a.get("path") else None
            tmp = Path(s.vimg_tmp).resolve()
            was_temp = old is not None and tmp in old.resolve().parents
            init_dir = s.vimg_dir if (was_temp or old is None) else old.parent
            name = "image" + vimg.EXT if was_temp or old is None else old.name
            dst = pick_save_vimg(name, init_dir or s.vimg_last)
            if not dst:
                return {"path": None}
            dst = dst.resolve()
            dst.parent.mkdir(parents=True, exist_ok=True)
            s.vimg_roots.add(dst.parent)
            doc = a["doc"]
            for l in doc.get("layers", []):
                if l.get("type") != "image" or not l.get("src"):
                    continue
                p = Path(l["src"])
                src = (p if p.is_absolute() or old is None else old.parent / p).resolve()
                if tmp in src.parents:  # 一時フォルダの取り込み画像は保存先の assets/ へ移す
                    folder = dst.parent / "assets"
                    folder.mkdir(exist_ok=True)
                    tgt, n = folder / src.name, 1
                    while tgt.exists():
                        n += 1
                        tgt = folder / f"{src.stem}_{n}{src.suffix}"
                    if src.is_file():
                        shutil.copy2(src, tgt)
                    l["src"] = "assets/" + tgt.name
                    continue
                try:
                    l["src"] = Path(os.path.relpath(src, dst.parent)).as_posix()
                except ValueError:
                    l["src"] = src.as_posix()
                s.vimg_refs.add(src)
            vimg.write_json(dst, doc)
            png = vimg.png_of(dst)
            if a.get("png"):
                vimg.write_png(png, a["png"])
            s.vimg_last = dst.parent
            return {"path": self._rel(dst), "png": self._rel(png), "doc": doc}
        raise ValueError(f"不明な操作: {act}")

    def do_POST(self):
        u = urlparse(self.path)
        if not self._ok_auth(parse_qs(u.query)):
            return self._json({"error": "unauthorized"}, 403)
        try:
            a = self._body()
            if u.path == "/api/save":
                p = self.proj
                if a.get("mtime") and not a.get("force") and p.path.exists() and abs(p.path.stat().st_mtime - float(a["mtime"])) > 1e-3:
                    return self._json({"error": "conflict"}, 409)  # 外部(CLI/AI)が先に書き換えた。黙って上書きしない
                p.data = a["project"]
                p.save()
                return self._json({"mtime": p.path.stat().st_mtime})
            if u.path == "/api/saveas":  # 別名で保存(ダイアログ)。test_path を渡すとダイアログを出さずにそこへ(テスト用)
                dst = a.get("test_path") or pick_save_project(self.proj.path)
                if not dst:
                    return self._json({"path": None})
                path = save_project_as(self.server, dst, a["project"])
                return self._json({"path": str(path), "name": path.stem})
            if u.path == "/api/job":
                p, kind = self.proj, a["kind"]
                fn = {"audio": lambda: job_audio(p, bool(a.get("force"))), "silence": lambda: job_silence(p, a),
                      "render": lambda: job_render(p, a), "frame": lambda: job_frame(p, a),
                      "proxy": lambda: (lambda prog: str(proxy.make(p, prog))),
                      "fx": lambda: (lambda prog: fx.preview(p, a["project"]))}[kind]()  # エフェクトのプレビュー(焼いた絵)
                return self._json({"id": start_job(fn)})
            if u.path == "/api/preview_audio":  # 試聴用: カット後の並びにつないだ処理済み音声(+効果音)
                p = self.proj
                for x in a.get("sfx") or []:
                    if not self._allowed(p.resolve(x["file"])):
                        return self._json({"error": f"プロジェクトにない効果音です: {x['file']}"}, 403)
                with PREVIEW_LOCK:
                    f = audio.preview_cut(p, a["pieces"], a.get("sfx"))
                return self._json({"path": str(f)})
            if u.path == "/api/review":  # 修正点: start / next(次の回) / add / update / delete / video。r = 回(省略で最新)
                p, op, r = self.proj, a["op"], a.get("r")
                if op == "start":
                    review.start(p, video=a.get("video"), force=bool(a.get("force")))
                    r = None
                elif op == "next":
                    review.next_round(p, video=a.get("video"))
                    r = None
                elif op == "add":
                    review.add(p, a["text"], r=r)
                elif op == "update":
                    review.update(p, a["i"], r=r, done=a.get("done"), text=a.get("text"), note=a.get("note"), t=a.get("t"))
                elif op == "delete":
                    review.delete(p, a["i"], r=r)
                elif op == "video":
                    review.set_video(p, a.get("video"), r=r)
                else:
                    raise ValueError(f"不明な操作: {op}")
                return self._json(review.state(p, r))
            if u.path == "/api/pick":
                if a["kind"] == "save":
                    p = self.proj
                    cur = p.resolve(a.get("name") or p.data["output"]["file"])
                    return self._json({"path": pick_save(cur.name, cur.parent if cur.parent.exists() else p.dir)})
                path = pick_file(a["kind"])
                if path:  # ダイアログで選んだファイルは保存前でも試聴・プレビューできるように
                    if not hasattr(self.server, "picked"):
                        self.server.picked = set()
                    self.server.picked.add(Path(path).resolve())
                if path and a["kind"] == "audio":  # 効果音を置くときの長さ
                    try:
                        return self._json({"path": path, "duration": ff.probe(path)["duration"]})
                    except Exception:
                        pass
                return self._json({"path": path})
            if u.path == "/api/photoshop":
                p = self.proj
                if a.get("action") == "open" and not self._allowed(p.resolve(a.get("path", ""))):
                    return self._json({"error": "プロジェクト外のファイルです"}, 403)
                return self._json({"path": photoshop(p, a)})
            if u.path.startswith("/api/vimg/"):
                return self._json(self._vimg(u.path[len("/api/vimg/"):], a))
            if u.path == "/api/open":  # 書き出し先フォルダをエクスプローラで開く
                target = Path(a["path"])
                if self._allowed(target) or self._allowed(target / "x"):
                    subprocess.Popen(["explorer", "/select,", str(target)] if target.is_file() else ["explorer", str(target)], creationflags=NORMAL)
                return self._json({"ok": True})
            self._json({"error": "not found"}, 404)
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as e:
            self._json({"error": str(e)}, 500)


def find_browser():
    cands = [shutil.which("msedge"), shutil.which("chrome")]
    for base in (os.environ.get("ProgramFiles(x86)"), os.environ.get("ProgramFiles"), os.environ.get("LOCALAPPDATA")):
        if base:
            cands += [Path(base) / "Microsoft/Edge/Application/msedge.exe", Path(base) / "Google/Chrome/Application/chrome.exe"]
    return next((str(c) for c in cands if c and Path(c).exists()), None)


def image_server(doc_path, out):
    """CLI の画像書き出し用(プロジェクト無し・文書のフォルダだけ見せる)。ページが PNG を送ると export_done が立つ。"""
    srv = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    srv.project_path, srv.token = None, secrets.token_urlsafe(16)
    srv.image_root, srv.image_refs = str(Path(doc_path).parent), vimg.refs(doc_path)
    srv.export_out, srv.export_done, srv.export_result = Path(out), threading.Event(), {}
    return srv


# サーバーは優先度を下げる(_background_priority)ので、そこから開く窓(Edge・エクスプローラー・単独の vimg 等)は通常に戻して起動する
NORMAL = getattr(subprocess, "NORMAL_PRIORITY_CLASS", 0)


def _background_priority():
    """GUI のサーバーは裏方なので優先度を「通常以下」にする(試聴用音声のミックス等でプレビュー再生・ブラウザをかくつかせない)。"""
    if os.name == "nt":
        try:
            import ctypes
            k = ctypes.windll.kernel32
            k.SetPriorityClass(k.GetCurrentProcess(), 0x4000)  # BELOW_NORMAL_PRIORITY_CLASS
        except Exception:
            pass


def serve(project_path, open_window=True, port=0):
    _background_priority()
    srv = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    srv.project_path, srv.token, srv.image_root = str(project_path), secrets.token_urlsafe(16), None
    url = f"http://127.0.0.1:{srv.server_address[1]}/"
    print(f"vedit GUI: {url}  (Ctrl+C で終了)", flush=True)
    if open_window:
        open_app_window(url)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


_pv_cache, _pv_lock = {}, threading.Lock()


_pvv_busy = set()


def preview_video(proj, src, audio=False):
    """差し込み動画のプレビュー用(proxy.make_video)。できていればそのパス、まだなら裏で作り始めて元のファイル。
    audio=True は音入り(修正点パネルの修正前の動画)。"""
    key = f"{src}|{audio}"
    try:
        out = proxy.video_proxy_path(proj, src, audio)
        if out.exists():
            return out
        with _pv_lock:
            if key in _pvv_busy:
                return src
            _pvv_busy.add(key)

        def run():
            try:
                proxy.make_video(proj, src, audio)
            except BaseException:
                pass
            finally:
                _pvv_busy.discard(key)
        threading.Thread(target=run, daemon=True).start()
    except Exception:
        pass
    return src


def preview_image(proj, src):
    """プレビュー用に縮めた画像(キャッシュ。Premiere のメディアキャッシュと同じ考え方)。
    4K の画像をそのまま出すと、表示のたびにデコードが重くてかくつく。キャンバス(1920x1080)に収まる大きさまで縮めて
    キャッシュに置き、GUI はそれを出す。書き出しは常に元の画像を使う。縮める必要が無い・失敗した時は元の画像を返す。"""
    try:
        st = src.stat()
        key = (str(src), st.st_mtime_ns, st.st_size)
        with _pv_lock:
            if key in _pv_cache and _pv_cache[key].exists():
                return _pv_cache[key]
        # 縮める作業は鍵の外で(1枚の縮小の間、ほかの画像の読み込みまで待たされないように)。同じ画像を同時に縮めても、一時ファイルを分けて最後に置き換えるだけ
        from PIL import Image
        h = hashlib.sha1("|".join(map(str, key)).encode()).hexdigest()[:16]
        jpg = src.suffix.lower() in (".jpg", ".jpeg")
        out = proj.cache_dir / "pvimg" / (h + (".jpg" if jpg else ".png"))
        if not out.exists():
            with Image.open(src) as im:
                k = min(1.0, max(1920 / im.width, 1080 / im.height))  # 幅か高さのどちらかはキャンバスいっぱいに出せる大きさ
                if k > 0.8:
                    with _pv_lock:
                        _pv_cache[key] = src
                    return src
                im = im.resize((max(1, round(im.width * k)), max(1, round(im.height * k))), Image.LANCZOS)
                out.parent.mkdir(parents=True, exist_ok=True)
                tmp = out.with_suffix(f".tmp{threading.get_ident()}" + out.suffix)
                if jpg:
                    im.convert("RGB").save(tmp, quality=90)
                else:
                    im.save(tmp, compress_level=1)
                os.replace(tmp, out)
        with _pv_lock:
            _pv_cache[key] = out
        return out
    except Exception:
        return src


def open_app_window(url, size="1500,900"):
    br = find_browser()
    profile = Path(os.environ.get("LOCALAPPDATA", ".")) / "vedit" / "browser"
    if br:  # アプリ風の専用ウィンドウ(タブやアドレスバー無し)
        # 拡張は読まない(Edge の同期で入った拡張が起動のたびに自分のページを開くため。2026-10-11 ユーザー指摘)
        subprocess.Popen([br, f"--app={url}", f"--user-data-dir={profile}", f"--window-size={size}",
                          "--autoplay-policy=no-user-gesture-required", "--disable-extensions",
                          "--no-first-run", "--no-default-browser-check"], creationflags=NORMAL)
    else:
        import webbrowser
        webbrowser.open(url)


# ---------------- vimg(画像編集の単独起動) ----------------
def launch_vimg(target=None):
    """別プロセスで `vedit_main.py vimg [target]` を起動する(動画編集GUIの「画像編集(vimg)を起動」用)。"""
    main = Path(__file__).resolve().parent.parent / "vedit_main.py"
    exe = sys.executable
    w = Path(exe).with_name("pythonw.exe")
    if w.exists():  # コンソール窓を出さない
        exe = str(w)
    cmd = [exe, str(main), "vimg"] + ([str(target)] if target else [])
    subprocess.Popen(cmd, cwd=str(main.parent), creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0) | NORMAL,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL)


def serve_vimg(target=None, open_window=True, port=0, idle=180):
    """画像編集(vimg)を動画プロジェクトなしで開く。target: .vimg.json / 画像 / フォルダ / None(新規・開くの画面)。
    閲覧・保存できるのは、開いたファイルのフォルダ・選んだフォルダ・保存先の配下と、開いた文書が参照する画像だけ。
    窓を閉じて idle 秒たつと自動で終了する。"""
    import tempfile
    from urllib.parse import quote
    srv = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    srv.project_path, srv.token, srv.image_root = None, secrets.token_urlsafe(16), None
    srv.vimg_mode, srv.vimg_tmp = True, tempfile.mkdtemp(prefix="vimg-")
    srv.vimg_roots, srv.vimg_refs = {Path(srv.vimg_tmp).resolve()}, set()
    srv.vimg_dir, srv.vimg_last, srv.vimg_ping = None, None, None
    query = ""
    if target:
        t = Path(target).resolve()
        if t.is_dir():
            srv.vimg_dir = str(t)
            srv.vimg_roots.add(t)
        elif t.is_file():
            if not (vimg.is_doc(t) or t.suffix.lower() in vimg.RASTER):
                raise SystemExit(f".vimg.json か画像を指定してください: {t}")
            srv.vimg_dir = str(t.parent)
            srv.vimg_roots.add(t.parent)
            query = "?f=" + quote(t.as_posix())
        else:
            raise SystemExit(f"見つかりません: {t}")
    url = f"http://127.0.0.1:{srv.server_address[1]}/{query}"
    print(f"vimg 画像編集: {url}  (Ctrl+C で終了)", flush=True)
    started = time.time()

    def watchdog():  # 窓が閉じられたら(生存確認が途絶えたら)終了
        while True:
            time.sleep(5)
            last = srv.vimg_ping or started
            if time.time() - last > (idle if srv.vimg_ping else max(idle, 600)):
                srv.shutdown()
                return

    if idle:
        threading.Thread(target=watchdog, daemon=True).start()
    if open_window:
        open_app_window(url, "1500,920")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        shutil.rmtree(srv.vimg_tmp, ignore_errors=True)
