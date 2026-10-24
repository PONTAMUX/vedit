"""GUI のプレビュー用の軽い映像(プロキシ。Premiere のプロキシと同じ考え方)。
収録データはキーフレームが数秒おき(例: 8.3秒)のことが多く、カットを飛ぶたびのシークで手前のキーフレームから
デコードし直すため映像が一瞬止まる。キーフレームを 0.5 秒おきにした 720p の映像をキャッシュに作り、GUI はそれを再生する。
書き出しは常に元の動画を使う。"""
import json
import os
import subprocess

from . import ff

VERSION = 1


def _sig(src):
    st = os.stat(src)
    return {"src": str(src), "mtime": st.st_mtime_ns, "size": st.st_size, "v": VERSION}


def ready(proj):
    """できていればプロキシのパス。無い・元の動画が変わった時は None。"""
    src = proj.resolve(proj.data["source"])
    out, meta = proj.cache_dir / "proxy.mp4", proj.cache_dir / "proxy.json"
    try:
        return out if out.exists() and json.loads(meta.read_text("utf-8")) == _sig(src) else None
    except (OSError, ValueError):
        return None


def make(proj, prog=None):
    if ready(proj):
        return ready(proj)
    src = proj.resolve(proj.data["source"])
    info = ff.probe(src)
    proj.cache_dir.mkdir(parents=True, exist_ok=True)
    out, meta, tmp = proj.cache_dir / "proxy.mp4", proj.cache_dir / "proxy.json", proj.cache_dir / "proxy.tmp.mp4"
    g = max(1, round((info.get("fps") or 30) / 2))  # 0.5 秒おきにキーフレーム
    if ff.has_encoder("h264_nvenc"):
        venc = ["-c:v", "h264_nvenc", "-preset", "p2", "-rc", "vbr", "-cq", "27", "-b:v", "0"]
    else:
        venc = ["-c:v", "libx264", "-preset", "veryfast", "-crf", "25"]
    args = [ff.find("ffmpeg"), "-hide_banner", "-y", "-loglevel", "error", "-progress", "pipe:1", "-nostats",
            "-i", str(src), "-map", "0:v:0", "-map", "0:a:0?", "-vf", "scale=-2:'min(720,ih)'", *venc,
            "-g", str(g), "-keyint_min", str(g), "-bf", "0", "-pix_fmt", "yuv420p",
            "-c:a", "aac", "-b:a", "160k", "-movflags", "+faststart", str(tmp)]
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0) | getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0)
    p = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8",
                         errors="replace", creationflags=flags)
    for line in p.stdout:
        if prog and line.startswith("out_time_us="):
            try:
                prog(min(1.0, int(line.split("=")[1]) / 1e6 / info["duration"]))
            except ValueError:
                pass
    err = p.stderr.read()
    if p.wait():
        tmp.unlink(missing_ok=True)
        raise SystemExit(f"プレビュー用の映像を作れませんでした: {err[-600:]}")
    os.replace(tmp, out)
    meta.write_text(json.dumps(_sig(src)), "utf-8")
    return out


# --- 差し込みの動画(ゲーム映像など)のプロキシ ---
# 切り出したクリップはキーフレームが頭の1枚だけのことがあり、プレビューで途中へ合わせるたびに頭からデコードし直して遅い。
# 0.5 秒おきにキーフレームを入れた 720p をキャッシュ(pvvid/)に作り、GUI はそれを出す。書き出しは元のファイル
def video_proxy_path(proj, src, audio=False):
    import hashlib
    st = os.stat(src)
    h = hashlib.sha1(f"{src}|{st.st_mtime_ns}|{st.st_size}|{VERSION}{'|a' if audio else ''}".encode()).hexdigest()[:16]
    return proj.cache_dir / "pvvid" / (h + ".mp4")


def make_video(proj, src, audio=False):
    """差し込み動画のプロキシを作って返す(できていればそのまま)。音は入れない(差し込みはプレビューで無音)。
    audio=True は音も入れる(修正点パネルの修正前の動画。「修正前の音を出す」で鳴らす)。"""
    out = video_proxy_path(proj, src, audio)
    if out.exists():
        return out
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix(".tmp.mp4")
    fps = ff.probe(src).get("fps") or 30
    g = max(1, round(fps / 2))
    venc = (["-c:v", "h264_nvenc", "-preset", "p2", "-rc", "vbr", "-cq", "27", "-b:v", "0"] if ff.has_encoder("h264_nvenc")
            else ["-c:v", "libx264", "-preset", "veryfast", "-crf", "25"])
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0) | getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0)
    r = subprocess.run([ff.find("ffmpeg"), "-hide_banner", "-y", "-loglevel", "error", "-i", str(src), "-map", "0:v:0",
                        *(["-map", "0:a:0?", "-c:a", "aac", "-b:a", "128k"] if audio else ["-an"]),
                        "-vf", "scale=-2:'min(720,ih)'", *venc, "-g", str(g), "-keyint_min", str(g), "-bf", "0",
                        "-pix_fmt", "yuv420p", "-movflags", "+faststart", str(tmp)],
                       capture_output=True, text=True, encoding="utf-8", errors="replace", creationflags=flags)
    if r.returncode:
        tmp.unlink(missing_ok=True)
        raise SystemExit(f"差し込み動画のプロキシを作れませんでした: {r.stderr[-400:]}")
    os.replace(tmp, out)
    return out
