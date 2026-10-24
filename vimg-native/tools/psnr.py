"""ref.png(Python版)と new.png(vimg.exe)の PSNR(アルファ込み、RGBA 4ch の MSE)。使い方: psnr.py [new名=new.png]"""
import sys, glob, os, numpy as np
from PIL import Image
name = sys.argv[1] if len(sys.argv) > 1 else "new.png"
res = []
for d in sorted(glob.glob("C:/Temp/vimgn/*/")):
    a, b = d + "ref.png", d + name
    if not (os.path.exists(a) and os.path.exists(b)): print(d, "missing"); continue
    A = np.asarray(Image.open(a).convert("RGBA"), dtype=np.float64); B = np.asarray(Image.open(b).convert("RGBA"), dtype=np.float64)
    if A.shape != B.shape: print(d, "size", A.shape, B.shape); continue
    # 透明部分の RGB は無意味なので、アルファで重みづけ(premultiplied)して比べる
    pa, pb = A.copy(), B.copy(); pa[..., :3] *= A[..., 3:] / 255; pb[..., :3] *= B[..., 3:] / 255
    mse = ((pa - pb) ** 2).mean()
    p = 99.0 if mse == 0 else 10 * np.log10(255 ** 2 / mse)
    nm = os.path.basename(os.path.normpath(d)); res.append((p, nm))
    print(f"{nm:6s} {p:6.2f} dB")
if res: print("min", min(res))
