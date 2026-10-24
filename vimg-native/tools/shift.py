import sys, numpy as np
from PIL import Image
n=sys.argv[1]; x0,y0,x1,y1=map(int,sys.argv[2:6])
P=30
A=np.pad(np.asarray(Image.open(f"C:/Temp/vimgn/{n}/ref.png").convert("RGBA"))[...,3].astype(float),P)
B=np.pad(np.asarray(Image.open(f"C:/Temp/vimgn/{n}/new.png").convert("RGBA"))[...,3].astype(float),P)
x0+=P;x1+=P;y0+=P;y1+=P
a=A[y0:y1,x0:x1]; best=None
for dy in range(-20,21):
  for dx in range(-20,21):
    b=B[y0+dy:y1+dy,x0+dx:x1+dx]; e=((a-b)**2).mean()
    if best is None or e<best[0]: best=(e,dx,dy)
print("new is shifted by (dx,dy) =",best[1:],"err",round(best[0],1),"vs zero",round(((a-B[y0:y1,x0:x1])**2).mean(),1))
