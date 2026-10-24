"""V-edit のアイコン(icon.png / icon.ico)を作る。usage: python make_icon.py"""
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageChops

S = 1024
HERE = Path(__file__).resolve().parent


def grad(w, h, c0, c1):
    """左上 c0 → 右下 c1 の斜めグラデーション"""
    y, x = np.mgrid[0:h, 0:w]
    t = ((x / (w - 1) + y / (h - 1)) / 2)[..., None]
    return Image.fromarray((np.array(c0) * (1 - t) + np.array(c1) * t).astype(np.uint8), "RGB")


# 下地: 角丸の紺(左上が少し明るい)
bg = grad(S, S, (32, 44, 96), (10, 12, 26))
mask = Image.new("L", (S, S), 0)
ImageDraw.Draw(mask).rounded_rectangle([0, 0, S - 1, S - 1], radius=220, fill=255)

# V(太い2本の線)。右の線は斜めに切って少しずらす(カット編集)
v = Image.new("L", (S, S), 0)
d = ImageDraw.Draw(v)
W = 150
L0, B, R0 = (250, 250), (512, 790), (774, 250)
d.line([L0, B], fill=255, width=W)
d.line([B, R0], fill=255, width=W)
d.ellipse([B[0] - W // 2, B[1] - W // 2, B[0] + W // 2, B[1] + W // 2], fill=255)
d.rectangle([0, 0, S, 205], fill=0)
# 切れ目: 右の線を斜めに切る
cut = Image.new("L", (S, S), 0)
ImageDraw.Draw(cut).polygon([(560, 420), (900, 330), (900, 356), (560, 446)], fill=255)
v = ImageChops.subtract(v, cut)
# 切った上側を右上へずらす
up = Image.new("L", (S, S), 0)
ImageDraw.Draw(up).polygon([(560, 420), (900, 330), (900, 0), (560, 0)], fill=255)
top = ImageChops.multiply(v, up)
v = ImageChops.subtract(v, top)
v = ImageChops.lighter(v, ImageChops.offset(top, 22, -6))

fill = grad(S, S, (110, 190, 255), (200, 90, 245))
# 光彩
glow = v.filter(ImageFilter.GaussianBlur(40)).point(lambda x: int(x * 0.55))
img = Image.composite(Image.new("RGB", (S, S), (110, 120, 255)), bg, glow)
img = Image.composite(fill, img, v)
# 上のふち(ハイライト)
hl = Image.new("L", (S, S), 0)
ImageDraw.Draw(hl).rounded_rectangle([6, 6, S - 7, S - 7], radius=214, outline=70, width=6)
img = Image.composite(Image.new("RGB", (S, S), (180, 190, 255)), img, hl.filter(ImageFilter.GaussianBlur(2)))

out = Image.new("RGBA", (S, S), (0, 0, 0, 0))
out.paste(img, (0, 0), mask)
out.resize((256, 256), Image.LANCZOS).save(HERE / "icon.png")
out.save(HERE / "icon.ico", sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
out.resize((512, 512), Image.LANCZOS).save(Path.home() / "AppData/Local/Temp/vedit_icon_preview.png")
print("ok")
