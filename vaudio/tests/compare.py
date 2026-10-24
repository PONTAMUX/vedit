"""C++ 版と Python 版の audio.wav を比べる。SNR(dB) = 10*log10(sum(ref^2)/sum((ref-test)^2))。
使い方: compare.py name [name ...]   (cpp/<name>/ と py/<name>/ のキャッシュを見る)"""
import json
import sys
import wave
from pathlib import Path

import numpy as np

ROOT = Path("C:/Temp/vaudio")


def read(p):
    with wave.open(str(p), "rb") as w:
        ch, sw, n = w.getnchannels(), w.getsampwidth(), w.getnframes()
        raw = np.frombuffer(w.readframes(n), np.uint8)
    if sw == 3:
        r = raw.reshape(-1, 3).astype(np.int32)
        v = (r[:, 0] | (r[:, 1] << 8) | (r[:, 2] << 16))
        v = np.where(v & 0x800000, v - (1 << 24), v)
        return v.reshape(-1, ch).astype(np.float64) / (1 << 23)
    raise SystemExit(f"sw={sw}")


for n in sys.argv[1:]:
    a = read(ROOT / "py" / n / f"{n}.vedit.cache" / "audio.wav")
    b = read(ROOT / "cpp" / n / f"{n}.vedit.cache" / "audio.wav")
    ja = json.load(open(ROOT / "py" / n / f"{n}.vedit.cache" / "audio.json", encoding="utf-8"))
    jb = json.load(open(ROOT / "cpp" / n / f"{n}.vedit.cache" / "audio.json", encoding="utf-8"))
    m = min(len(a), len(b))
    if len(a) != len(b):
        print(f"{n}: 長さが違う py={len(a)} cpp={len(b)}")
    d = a[:m] - b[:m]
    e = float((d ** 2).sum())
    s = float((a[:m] ** 2).sum())
    snr = 10 * np.log10(s / e) if e > 0 else float("inf")
    print(f"{n}: SNR={snr:.1f} dB maxdiff={np.abs(d).max():.3e} (LSB={np.abs(d).max() * (1 << 23):.1f})")
    for k in ("peak_before_norm_db", "norm_gain_db", "pre_gain_db", "lufs_before", "lufs_after", "clicks_repaired"):
        flag = "" if ja.get(k) == jb.get(k) else "   <-- 違う"
        print(f"   {k}: py={ja.get(k)} cpp={jb.get(k)}{flag}")
