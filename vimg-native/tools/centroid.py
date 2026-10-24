import sys, numpy as np
from PIL import Image
n=sys.argv[1]; x0,y0,x1,y1=map(int,sys.argv[2:6])
def g(f):
    im=np.asarray(Image.open(f"C:/Temp/vimgn/{n}/{f}").convert("RGBA")).astype(float)
    # 白い部分(文字の塗り・内縁)の重心: 明るさ*アルファ
    w=(im[...,:3].mean(-1)*im[...,3]/255)[y0:y1,x0:x1]; ys,xs=np.mgrid[y0:y1,x0:x1]
    return (w*xs).sum()/w.sum(),(w*ys).sum()/w.sum()
a=g("ref.png");b=g("new.png");print("ref",a,"new",b,"new-ref dx,dy",round(b[0]-a[0],3),round(b[1]-a[1],3))
