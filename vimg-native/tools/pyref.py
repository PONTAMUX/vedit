"""Python 版 vedit image で基準 PNG(ref.png)を作る。"""
import subprocess, sys, glob, os, time
for d in sorted(glob.glob("C:/Temp/vimgn/*/")):
    if os.path.exists(d + "ref.png") and "-f" not in sys.argv: continue
    t = time.time()
    r = subprocess.run([sys.executable, "C:/vedit/vedit_main.py", "image", d + "doc.vimg.json", "-o", d + "ref.png"], capture_output=True, text=True, creationflags=0x08000000)
    print(d, round(time.time() - t, 1), r.returncode, (r.stdout + r.stderr).strip()[-150:], flush=True)
