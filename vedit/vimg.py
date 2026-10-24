"""画像編集(Photoshop の代わり)の補助。編集データは *.vimg.json、仕上がりは同名の透過PNG。

描画はブラウザ(web/image.js)だけが行う。CLI の書き出しも Edge headless で同じページを開いて描かせる。
"""
import base64
import json
import os
import shutil
import struct
import subprocess
import tempfile
import threading
import zlib
from pathlib import Path
from urllib.parse import quote

EXT = ".vimg.json"
RASTER = (".png", ".jpg", ".jpeg", ".webp", ".bmp", ".gif")


def is_doc(p):
    return str(p).lower().endswith(EXT)


def png_of(doc):
    """a/b.vimg.json → a/b.png"""
    s = str(doc)
    return Path(s[: -len(EXT)] + ".png") if is_doc(s) else Path(s).with_suffix(".png")


def blank_png(w, h):
    """完全に透明な RGBA PNG(標準ライブラリだけで作る)。"""
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    raw = (b"\x00" + b"\x00" * (4 * w)) * h
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def new_doc(w=1920, h=1080, layers=None, background=None):
    return {"version": 1, "width": w, "height": h, "background": background, "layers": layers or []}


def telop_layer(st, W, H):
    """project のテロップスタイル(1080p基準)→ テキストレイヤー。位置も本体のテロップと同じ辺りに置く。"""
    k = H / 1080
    size = round(st["size"] * k)
    outs = [{"color": o["color"], "width": round(o["width"] * k, 1)} for o in st.get("outlines", [])]
    pad = sum(o["width"] for o in outs)
    hh = size * 1.25 + pad * 2
    mv = st.get("margin_v", 70) * k
    y = {"top": mv + hh / 2, "center": H / 2}.get(st.get("align", "bottom"), H - mv - hh / 2)
    return {"type": "text", "name": "テロップ", "text": "テロップ", "x": round(W / 2), "y": round(y), "rot": 0,
            "font": st["font"], "size": size, "color": st["color"], "bold": bool(st.get("bold")),
            "line_height": 1.25, "letter_spacing": 0, "scale_x": 100, "max_w": 0, "align": "center", "valign": "middle",
            "outlines": outs, "shadow": None, "gradient": None, "auto": True}


def unique(folder, stem):
    """folder/stem_01.vimg.json のうち、json も png もまだ無い番号のパス。"""
    folder.mkdir(parents=True, exist_ok=True)
    n = 1
    while (folder / f"{stem}_{n:02d}{EXT}").exists() or (folder / f"{stem}_{n:02d}.png").exists():
        n += 1
    return folder / f"{stem}_{n:02d}{EXT}"


def write_json(path, doc):
    tmp = Path(str(path) + ".tmp")
    tmp.write_text(json.dumps(doc, ensure_ascii=False, indent=2), encoding="utf-8")
    os.replace(tmp, path)


def write_png(path, b64):
    if "," in b64[:64]:  # data:image/png;base64,... のままでもよい
        b64 = b64.split(",", 1)[1]
    data = base64.b64decode(b64)
    if not data.startswith(b"\x89PNG"):
        raise ValueError("PNG ではありません")
    tmp = Path(str(path) + ".tmp")
    tmp.write_bytes(data)
    os.replace(tmp, path)


def create(doc_path, doc):
    """文書と(まだ無ければ)透明PNGを書く。PNG を先に置いておけば差し込みの検証が通る。"""
    write_json(doc_path, doc)
    png = png_of(doc_path)
    if not png.exists():
        png.write_bytes(blank_png(int(doc["width"] or 1920), int(doc["height"] or 1080)))
    return png


def from_raster(src):
    """既存の画像を1枚目のレイヤーにした文書を作る(隣に <名前>.vimg.json)。
    PNG はこの文書の書き出しで上書きされるので、元画像は <名前>.src.<拡張子> に退避して参照する。"""
    src = Path(src)
    doc_path = src.with_name(src.stem + EXT)
    if doc_path.exists():
        return doc_path
    if src.suffix.lower() != ".png" and png_of(doc_path).exists():  # a.jpg の隣に無関係の a.png がある → a_jpg.vimg.json(→ a_jpg.png)
        doc_path = src.with_name(f"{src.stem}_{src.suffix[1:].lower()}{EXT}")
        if doc_path.exists():
            return doc_path
    keep = src.with_name(f"{src.stem}.src{src.suffix.lower()}")
    if not keep.exists():
        shutil.copy2(src, keep)
    layer = {"type": "image", "name": src.name, "src": keep.name, "x": None, "y": None, "w": None, "h": None, "rot": 0}
    write_json(doc_path, new_doc(None, None, [layer]))  # サイズは画像に合わせる(ページ側で決める)
    return doc_path


def refs(doc_path):
    """文書が参照している画像の絶対パス(書き出し時の閲覧許可に使う)。"""
    d = json.loads(Path(doc_path).read_text("utf-8"))
    out = []
    for l in d.get("layers", []):
        if l.get("type") == "image" and l.get("src"):
            p = Path(l["src"])
            out.append((p if p.is_absolute() else Path(doc_path).parent / p).resolve())
    return out


def export(doc_path, out=None, timeout=90):
    """Edge headless で image.html を書き出し専用モードで開き、ページが描いた PNG を受け取る。"""
    from . import server
    doc_path = Path(doc_path).resolve()
    if not doc_path.is_file():
        raise SystemExit(f"ファイルがありません: {doc_path}")
    out = Path(out).resolve() if out else png_of(doc_path)
    br = server.find_browser()
    if not br:
        raise SystemExit("Edge / Chrome が見つかりません")
    srv = server.image_server(doc_path, out)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    url = f"http://127.0.0.1:{srv.server_address[1]}/image.html?f={quote(doc_path.name)}&export=1"
    tmp = tempfile.mkdtemp(prefix="vedit-img-")
    proc = subprocess.Popen([br, "--headless=new", "--disable-gpu", "--no-first-run", "--hide-scrollbars",
                             f"--user-data-dir={tmp}", "--window-size=800,600", url],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    try:
        if not srv.export_done.wait(timeout):
            raise SystemExit(f"{timeout}秒以内に書き出しが終わりませんでした")
    finally:
        subprocess.run(["taskkill", "/T", "/F", "/PID", str(proc.pid)], capture_output=True, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        srv.shutdown()
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            pass
        shutil.rmtree(tmp, ignore_errors=True)
    if srv.export_result.get("error"):
        raise SystemExit("書き出し失敗: " + srv.export_result["error"])
    return {"output": str(out), **{k: v for k, v in srv.export_result.items() if k in ("width", "height")}}
