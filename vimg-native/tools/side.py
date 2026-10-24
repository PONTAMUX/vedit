import sys, numpy as np
from PIL import Image
n = sys.argv[1]; d = f"C:/Temp/vimgn/{n}/"
bg = (60, 60, 60, 255)
def L(f):
    im = Image.open(d + f).convert("RGBA"); b = Image.new("RGBA", im.size, bg); b.alpha_composite(im); return b.convert("RGB")
a, b = L("ref.png"), L("new.png")
diff = Image.fromarray(np.clip(np.abs(np.asarray(a, int) - np.asarray(b, int)) * 3, 0, 255).astype("uint8"))
w, h = a.size; s = min(1, 700 / w)
m = Image.new("RGB", (int(w * s) * 3, int(h * s)))
for i, im in enumerate((a, b, diff)): m.paste(im.resize((int(w * s), int(h * s))), (i * int(w * s), 0))
m.save(f"C:/Temp/vimgn/side_{n}.png")
