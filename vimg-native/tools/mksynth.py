"""image.js の全種類・全機能を使った合成テスト文書を C:/Temp/vimgn/x01.. に作る。"""
import json, os
from PIL import Image, ImageDraw
OUT = "C:/Temp/vimgn"
def asset(d):
    os.makedirs(d + "/assets", exist_ok=True)
    im = Image.new("RGBA", (400, 300))
    px = im.load()
    for y in range(300):
        for x in range(400): px[x, y] = (x * 255 // 400, y * 255 // 300, 128, 255)
    dr = ImageDraw.Draw(im); dr.ellipse((100, 60, 300, 240), fill=(250, 200, 30, 255)); dr.rectangle((0, 0, 60, 60), fill=(200, 30, 30, 128))
    im.save(d + "/assets/p.png")
    im.convert("RGB").save(d + "/assets/p.jpg", quality=92)
def put(n, doc, img=False):
    d = f"{OUT}/{n}"; os.makedirs(d, exist_ok=True)
    if img: asset(d)
    json.dump(doc, open(d + "/doc.vimg.json", "w", encoding="utf-8"), ensure_ascii=False, indent=2)
T = lambda **k: {"type": "text", **k}
# x01: 図形・縁・影・回転・不透明度
put("x01", {"version": 1, "width": 1280, "height": 720, "background": "#203040", "layers": [
    {"type": "shape", "shape": "rect", "x": 300, "y": 200, "w": 400, "h": 240, "fill": "#3B82F6", "stroke": {"color": "#FFFFFF", "width": 10}, "rot": 15},
    {"type": "shape", "shape": "roundrect", "x": 800, "y": 200, "w": 400, "h": 240, "radius": 60, "fill": "#E5484D", "stroke": None, "opacity": 0.6, "shadow": {"color": "#000000", "opacity": 0.7, "x": 12, "y": 12, "blur": 16}},
    {"type": "shape", "shape": "ellipse", "x": 400, "y": 520, "w": 360, "h": 200, "fill": None, "stroke": {"color": "#FFE066", "width": 14}, "rot": -20},
    {"type": "shape", "shape": "ellipse", "x": 900, "y": 540, "w": 240, "h": 240, "fill": "#22C55E", "stroke": {"color": "#000000", "width": 6}, "shadow": {"color": "#FF0000", "opacity": 1, "x": -10, "y": 8, "blur": 0}},
    {"type": "shape", "shape": "rect", "x": 640, "y": 360, "w": 100, "h": 100, "fill": "#FFFFFF", "visible": False},
    {"type": "shape", "shape": "rect", "x": 100, "y": 100, "w": 100, "h": 100, "fill": "#FF00FF", "export": False},
]})
# x02: 文字いろいろ
put("x02", {"version": 1, "width": 1920, "height": 1080, "background": None, "layers": [
    T(text="テロップ既定の文字", x=960, y=150),
    T(text="グラデーション\n二行目です", x=500, y=450, size=96, gradient={"top": "#FFF27A", "bottom": "#FF9A1F"}, outlines=[{"color": "#000000", "width": 4}, {"color": "#FFFFFF", "width": 6}, {"color": "#E5484D", "width": 8}], shadow={"color": "#000000", "opacity": 0.6, "x": 6, "y": 6, "blur": 8}),
    T(text="左寄せ 字間20 太字", x=1400, y=450, size=72, align="left", letter_spacing=20, bold=True, color="#7CFF7C", rot=-8),
    T(text="右寄せ横比率70%\n最大幅で縮む長い長い長い長い行です", x=960, y=800, size=80, align="right", scale_x=70, max_w=900, line_height=1.5),
    T(text="Latin ABC xyz 123 gj", x=300, y=950, size=60, font="Yu Gothic UI", color="#FFFFFF", outlines=[], opacity=0.8),
    T(text="固定枠 上", x=1500, y=850, w=500, h=300, auto=False, size=60, valign="top", align="center"),
    T(text="固定枠 下", x=1500, y=980, w=500, h=200, auto=False, size=60, valign="bottom", align="left"),
]})
# x03: 画像
put("x03", {"version": 1, "width": 1000, "height": 700, "background": "#FFFFFF", "layers": [
    {"type": "image", "src": "assets/p.png", "x": 250, "y": 200, "w": 400, "h": 300},
    {"type": "image", "src": "assets/p.png", "x": 750, "y": 200, "w": 300, "h": 225, "flip": True, "rot": 12, "shadow": {"color": "#000000", "opacity": 0.6, "x": 8, "y": 8, "blur": 10}},
    {"type": "image", "src": "assets/p.jpg", "x": 250, "y": 520, "w": 200, "h": 150, "adjust": {"brightness": 30, "contrast": 40, "saturation": -60}},
    {"type": "image", "src": "assets/p.jpg", "x": 500, "y": 520, "w": 200, "h": 150, "adjust": {"brightness": -20, "contrast": -30, "saturation": 80}, "opacity": 0.7},
    {"type": "image", "src": "assets/p.png", "w": 120, "h": 90},
    T(text="画像の上", x=750, y=550, size=70),
], })
# x04: 画像サイズ省略(原寸・中央)・文書サイズ省略
put("x04", {"version": 1, "layers": [{"type": "image", "src": "assets/p.png", "x": None, "y": None, "w": None, "h": None, "rot": 0}]}, img=True)
put("x03", json.load(open(OUT + "/x03/doc.vimg.json", encoding="utf-8")), img=True)
