"""V-edit の自前の書き出しエンジン(重ね合わせだけを自分でする)。
ffmpeg は部品として使う: 背景(カット・揺れ・黒)を作る ffmpeg、差し込みをデコード・縮小する ffmpeg、
テロップ(ASS)を焼いてエンコードする ffmpeg を立ち上げ、その間の重ね合わせを GPU(PyTorch / CUDA)でする。
(OBS と同じ作り。ffmpeg のフィルタグラフの重ね合わせは差し込みが増えると重く、GPU 版は色空間などの制限で失敗しやすい)
差し込みは出番の少し前に読み始め、出番が終わったら閉じる(出ていない素材は読まない)。
背景の受け取り・差し込みの受け取り・エンコーダーへの書き込みは別々のスレッドで同時に進める(流れ作業)。

使い方:
  python engine.py --serve          書き出し1回ぶんの常駐(render.py が使う。torch の起動は数秒かかるので1回だけにする)
      標準入力に1行1件 {"k": 区間番号, "job": 指示.json, "out": 出力.mp4}。区間ごとに並列で作る。
      標準出力に "k frame=N" / "k ok" / "k err 理由"
  python engine.py 指示.json 出力.mp4   1区間だけ(試験用)
重ね方は ffmpeg の overlay(format=yuv420)と同じ: 位置は偶数に切り捨て、色差は透明度を 2x2 で平均、(d*(255-a)+s*a)/255。
"""
import json
import os
import queue
import subprocess
import sys
import threading

NOWIN = 0x08000000
PREFETCH = 2.0  # 差し込みを出番の何秒前から読み始めるか
POOL = 6  # 背景・差し込みごとに先読みしておくコマ数
PIPE = 1 << 22  # パイプの大きさ。Windows の既定(4KB)だと 1コマ 3MB を何百回にも分けて読み、Python が詰まる


def _pipe():
    """(読む側の fd, 書く側の fd)。Windows では大きなパイプを作る"""
    if os.name != "nt":
        return os.pipe()
    import _winapi
    import msvcrt
    r, w = _winapi.CreatePipe(None, PIPE)
    return msvcrt.open_osfhandle(r, os.O_RDONLY), msvcrt.open_osfhandle(w, 0)


def readfull(f, mv):
    """mv をいっぱいまで読む。読めたバイト数(途中で終われば少ない)"""
    n, size = 0, len(mv)
    while n < size:
        r = f.readinto(mv[n:])
        if not r:
            break
        n += r
    return n


class Proc:
    """ffmpeg を1本立ち上げて、エラー出力を裏で読む(詰まらないように)。"""

    def __init__(self, ffmpeg, args, cwd, stdin=False, stdout=False):
        ir, iw = _pipe() if stdin else (subprocess.DEVNULL, None)
        or_, ow = _pipe() if stdout else (None, subprocess.DEVNULL)
        try:
            self.p = subprocess.Popen([ffmpeg, "-hide_banner", "-loglevel", "error", *([] if stdin else ["-nostdin"]), *map(str, args)],
                                      cwd=cwd, stdin=ir, stdout=ow, stderr=subprocess.PIPE, creationflags=NOWIN, bufsize=0)
        finally:  # 子に渡した側はこちらでは閉じる
            for fd in (ir if stdin else None, ow if stdout else None):
                if fd is not None:
                    os.close(fd)
        self.inp = open(iw, "wb", buffering=0) if stdin else None
        self.out = open(or_, "rb", buffering=0) if stdout else None
        self.err = []
        self.th = threading.Thread(target=lambda: self.err.append(self.p.stderr.read()), daemon=True)
        self.th.start()

    def errtext(self):
        self.th.join(3)
        return b"".join(self.err).decode("utf-8", "replace")[-600:]

    def kill(self):
        if self.p.poll() is None:
            self.p.kill()
        self.p.wait()
        for f in (self.inp, self.out):
            if f:
                try:
                    f.close()
                except OSError:
                    pass


class Pins:
    """先読みのメモリ(GPU へ速く送れるページ固定メモリ)を大きさごとに使い回す。
    確保は遅く(1つ数 ms)GPU とも待ち合わせるので、差し込みのたびに取り直さない"""

    def __init__(self, torch, pin):
        self.torch, self.pin, self.lock, self.bufs = torch, pin, threading.Lock(), {}

    def get(self, size):
        with self.lock:
            lst = self.bufs.get(size)
            if lst:
                return lst.pop()
        return self.torch.empty(size, dtype=self.torch.uint8, pin_memory=self.pin)

    def give(self, buf):
        with self.lock:
            self.bufs.setdefault(buf.numel(), []).append(buf)


class Reader:
    """ffmpeg の生の絵を裏のスレッドで先読みする。get() → 絵(読み終わったら None)。
    使い終わった絵は put_back で返す(閉じた後に返されたものは Pins へ)"""

    def __init__(self, pins, proc, size, count):
        self.pins, self.proc, self.size, self.count = pins, proc, size, count
        self.closed = False
        self.free = queue.Queue()
        for _ in range(min(POOL, count)):
            self.free.put(pins.get(size))
        self.q = queue.Queue()
        self.th = threading.Thread(target=self.loop, daemon=True)
        self.th.start()

    def loop(self):
        try:
            for _ in range(self.count):
                buf = self.free.get()
                if buf is None:
                    break
                if readfull(self.proc.out, memoryview(buf.numpy())) < self.size:
                    self.free.put(buf)
                    break
                self.q.put(buf)
        except (OSError, ValueError):
            pass
        self.q.put(None)

    def get(self):
        return self.q.get()

    def put_back(self, buf):
        if self.closed:
            self.pins.give(buf)
        else:
            self.free.put(buf)

    def close(self):
        if self.closed:
            return
        self.closed = True
        self.free.put(None)
        self.proc.kill()
        self.th.join(5)
        for q in (self.free, self.q):  # 読んだけれど使わなかった絵・空きを返す
            while True:
                try:
                    b = q.get_nowait()
                except queue.Empty:
                    break
                if b is not None:
                    self.pins.give(b)


class Layer:
    """差し込み1つ。出番の少し前に ffmpeg を立ち上げて先読みする。
    透明のない動画(L["alpha"] が False)は yuv420p でもらい、重ねずにそのまま写す(下は丸ごと隠れる)"""

    def __init__(self, L, job, dev, torch, pins):
        self.L, self.job, self.dev, self.torch, self.pins = L, job, dev, torch, pins
        self.alpha = L.get("alpha", True)
        w, h = L["w"], L["h"]
        self.size = w * h * (5 if self.alpha else 3) // 2  # Y + U/4 + V/4 (+ A)
        self.rd = self.g = self.fixed = self.cur = None
        self.have = self.done = self.up = self.handed = False
        self.solid = not self.alpha  # 透明なところがない(画像は読んでから調べる)
        self.ox = self.oy = 0  # 画像は透明でないところだけに切り詰める。その左上のずれ
        self.empty = False  # 全部透明(何も描かない)

    def start(self):
        if self.rd is None and not self.done:
            L = self.L
            n = 1 if L["img"] else L["m1"] - L["m0"] + 1
            p = Proc(self.job["ffmpeg"], [*L["dec"], "-fps_mode", "passthrough", "-f", "rawvideo",
                                          "-pix_fmt", "yuva420p" if self.alpha else "yuv420p", "-"], self.job["cwd"], stdout=True)
            self.rd = Reader(self.pins, p, self.size, n)

    def fetch(self, stream):
        """次の絵を受け取る(GPU へはまだ送らない)。読めなければ False(動画が終わった = それ以降は出さない)。
        前の絵のメモリはここで返す(前のコマの GPU への送りは、コマの終わりの synchronize で済んでいる)"""
        if self.done:
            return False
        if self.L["img"] and self.have:
            return True
        self.start()
        buf = self.rd.get()
        if buf is None:
            code = self.rd.proc.p.wait()
            if code or not self.have:
                raise RuntimeError(f"差し込みを読めませんでした {self.L['file']}: {self.rd.proc.errtext()}")
            self.close()
            return False
        if self.cur is not None and not self.handed:
            self.rd.put_back(self.cur)
        self.cur, self.handed, self.up, self.have = buf, False, False, True
        if self.L["img"]:  # 画像は1回だけ送って、透明なところがあるか調べておく
            Yo, Uo, Vo, Ao = self.gpu()
            self.solid = bool((Ao == 255).all())
            if not self.solid:  # 全画面のテロップ画像などは、透明でない四角(偶数にそろえる)だけを重ねる
                rows, cols = Ao.any(1).nonzero(), Ao.any(0).nonzero()
                if not len(rows):
                    self.empty = True
                else:
                    y0, y1 = int(rows[0]) & ~1, min(Ao.shape[0], (int(rows[-1]) + 2) & ~1)
                    x0, x1 = int(cols[0]) & ~1, min(Ao.shape[1], (int(cols[-1]) + 2) & ~1)
                    self.ox, self.oy = x0, y0
                    self.planes = (Yo[y0:y1, x0:x1], Uo[y0 // 2:y1 // 2, x0 // 2:x1 // 2], Vo[y0 // 2:y1 // 2, x0 // 2:x1 // 2], Ao[y0:y1, x0:x1])
            if stream:
                stream.synchronize()
            self.rd.put_back(self.cur)
            self.cur = None
            self.rd.close()
        return True

    def gpu(self):
        """今の絵を GPU の平面 (Y, U, V[, A]) で"""
        if self.g is None:
            w, h, q, t = self.L["w"], self.L["h"], self.L["w"] * self.L["h"] // 4, self.torch
            self.g = g = t.empty(self.size, dtype=t.uint8, device=self.dev)
            self.planes = (g[:w * h].view(h, w), g[w * h:w * h + q].view(h // 2, w // 2),
                           g[w * h + q:w * h + 2 * q].view(h // 2, w // 2)) + ((g[w * h + 2 * q:].view(h, w),) if self.alpha else ())
        if not self.up and self.cur is not None:
            self.g.copy_(self.cur, non_blocking=True)
            self.up = True
        return self.planes

    def hand(self):
        """今の絵のメモリをそのまま書き込みへ渡す(全画面・透明なし・いちばん上のとき)。→ (絵, 返し先)"""
        self.handed = True
        rd = self.rd
        return self.cur, (rd.put_back if rd else (lambda b: None))

    def prep(self, f):
        """重ねる材料 (Y, U, V, 輝度の透明度, 色差の透明度)(int32)。f はフェードの掛け率(None=なし)。
        画像でフェードのないコマは1回だけ作って使い回す"""
        if self.fixed is not None and f is None:
            return self.fixed
        t = self.torch
        Yo, Uo, Vo, *A = self.gpu()
        a = A[0].to(t.int32) if A else t.full(Yo.shape, 255, dtype=t.int32, device=self.dev)
        if f is not None:
            a = (a * f + 32768) >> 16
        ac = (a[0::2, 0::2] + a[0::2, 1::2] + a[1::2, 0::2] + a[1::2, 1::2]) >> 2
        r = (Yo.to(t.int32), Uo.to(t.int32), Vo.to(t.int32), a, ac)
        if self.L["img"] and f is None:
            self.fixed = r
        return r

    def close(self):
        if self.rd:
            if self.cur is not None and not self.handed:
                self.rd.put_back(self.cur)
            self.cur = None
            self.rd.close()
            self.rd = None
        self.done, self.g, self.fixed = True, None, None


def clip01(x):
    return 0.0 if x < 0 else 1.0 if x > 1 else x


def pos_at(L, t, W, H):
    """位置(build の overlay の x/y の式と同じ計算)。"""
    x, y, w, h, dur = L["x"], L["y"], L["w"], L["h"], L["dur"]
    p_in = 1 - clip01((t - L["a"]) / dur) if dur > 0 else 0.0
    p_out = clip01((t - L["bd"]) / dur) if dur > 0 else 0.0
    for kind, p in ((L["in"], p_in), (L["out"], p_out)):
        if kind == "slide-left":
            x = x - p * (x + w)
        elif kind == "slide-right":
            x = x + p * (W - x)
        elif kind == "slide-up":
            y = y + p * (H - y)
        elif kind == "slide-down":
            y = y - p * (y + h)
    return int(x) & ~1, int(y) & ~1  # overlay(yuv420)は小数を切り捨てて偶数にそろえる


def fade_at(L, t):
    """fade=alpha=1 の掛け率(0〜65535)。None は掛けない"""
    f = None
    d = L["dur"]
    if L["in"] == "fade":
        f = 65535 if d <= 0 or t >= L["a"] + d else max(0, int((t - L["a"]) * 65535 / d))
    if L["out"] == "fade":
        g = 0 if d <= 0 or t >= L["bd"] + d else min(65535, 65535 - max(0, int((t - L["bd"]) * 65535 / d)))
        f = g if f is None else (f * g) // 65535
    return f


def blend(torch, dst, src, alpha, x, y):
    """dst(平面)の (x,y) に src を alpha(int32、src と同じ大きさ)で重ねる。画面の外にはみ出たぶんは切る。"""
    H, W = dst.shape
    h, w = src.shape
    x0, y0, x1, y1 = max(0, x), max(0, y), min(W, x + w), min(H, y + h)
    if x1 <= x0 or y1 <= y0:
        return
    s = src[y0 - y:y1 - y, x0 - x:x1 - x]
    d = dst[y0:y1, x0:x1]
    a = alpha[y0 - y:y1 - y, x0 - x:x1 - x]
    di = d.to(torch.int32)
    v = (s - di) * a + di * 255  # = d*(255-a) + s*a(src・alpha は int32)
    d.copy_((((v + 128) * 257) >> 16).to(torch.uint8))  # ffmpeg の FAST_DIV255


def put(dst, src, x, y):
    """dst(平面)の (x,y) に src をそのまま写す(透明のないもの)。画面の外にはみ出たぶんは切る。"""
    H, W = dst.shape
    h, w = src.shape
    x0, y0, x1, y1 = max(0, x), max(0, y), min(W, x + w), min(H, y + h)
    if x1 > x0 and y1 > y0:
        dst[y0:y1, x0:x1].copy_(src[y0 - y:y1 - y, x0 - x:x1 - x])


def run(job, out, progress=lambda n: None, torch=None):
    if torch is None:
        import torch
    cuda = torch.cuda.is_available() and job.get("gpu", True)
    dev = torch.device("cuda" if cuda else "cpu")
    W, H, F, N, M = job["W"], job["H"], job["F"], job["N"], job["m"]
    fs = W * H * 3 // 2
    pins = Pins(torch, cuda)
    layers = [Layer(L, job, dev, torch, pins) for L in job["layers"]]
    base = Proc(job["ffmpeg"], [*job["base"], "-fps_mode", "passthrough", "-f", "rawvideo", "-pix_fmt", "yuv420p", "-"], job["cwd"], stdout=True)
    enc = Proc(job["ffmpeg"], ["-y", "-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", f"{W}x{H}", "-framerate", job["rate"], "-i", "-",
                               *job["enc"], out], job["cwd"], stdin=True)
    brd = Reader(pins, base, fs, N)
    ofree = queue.Queue()  # 合成した絵を書き込みへ渡すページ固定メモリ(背景をそのまま渡すときは背景のものを返してもらう)
    for _ in range(3):
        ofree.put(torch.empty(fs, dtype=torch.uint8, pin_memory=cuda))
    wq = queue.Queue(maxsize=POOL)
    werr = []

    def writer():
        try:
            while True:
                it = wq.get()
                if it is None:
                    break
                buf, back = it
                enc.inp.write(memoryview(buf.numpy()))
                back(buf)
        except (OSError, ValueError) as e:
            werr.append(e)

    wt = threading.Thread(target=writer, daemon=True)
    wt.start()
    stream = torch.cuda.Stream() if cuda else None  # 区間ごとに別の流れ(並列の区間どうしで待ち合わせない)
    g = torch.empty(fs, dtype=torch.uint8, device=dev)
    Y, U, V = g[:W * H].view(H, W), g[W * H:W * H * 5 // 4].view(H // 2, W // 2), g[W * H * 5 // 4:].view(H // 2, W // 2)
    pre = round(PREFETCH * F)
    try:
        with torch.cuda.stream(stream) if cuda else _null():
            for n in range(N):
                if werr:
                    raise RuntimeError(f"エンコーダーに渡せませんでした: {enc.errtext()}")
                m = M + n
                t = m / F
                act = []
                for ly in layers:
                    L = ly.L
                    if ly.done:
                        continue
                    if m > L["m1"]:
                        ly.close()
                        continue
                    if m >= L["m0"] - pre:
                        ly.start()  # 先に読み始めておく(ffmpeg の立ち上がりで止まらないように)
                    if L["m0"] <= m and ly.fetch(stream) and not ly.empty:
                        act.append(ly)
                b = brd.get()
                if b is None:
                    base.p.wait()
                    raise RuntimeError(f"背景が {n} コマで終わりました: {base.errtext()}")
                if not act:
                    wq.put((b, brd.put_back))
                else:
                    # 位置・フェード。上から見て、画面を丸ごと隠す不透明なもの(s)より下は描かない
                    st = []
                    for ly in act:
                        x, y = pos_at(ly.L, t, W, H)
                        x, y = x + ly.ox, y + ly.oy
                        f = fade_at(ly.L, t)
                        st.append((ly, x, y, f, ly.solid and f is None))
                    s0 = None
                    for i in range(len(st) - 1, -1, -1):
                        ly, x, y, f, op = st[i]
                        if op and x <= 0 and y <= 0 and x + ly.L["w"] >= W and y + ly.L["h"] >= H:
                            s0 = i
                            break
                    if s0 == len(st) - 1 and not st[s0][0].L["img"] and (st[s0][1], st[s0][2], st[s0][0].L["w"], st[s0][0].L["h"]) == (0, 0, W, H):
                        brd.put_back(b)  # いちばん上が全画面の不透明な動画なら、その絵をそのまま渡す(GPU を通さない)
                        wq.put(st[s0][0].hand())
                    else:
                        if s0 is None:
                            g.copy_(b, non_blocking=True)
                        for ly, x, y, f, op in st[s0 or 0:]:
                            if op:
                                Yo, Uo, Vo = ly.gpu()[:3]
                                put(Y, Yo, x, y)
                                put(U, Uo, x // 2, y // 2)
                                put(V, Vo, x // 2, y // 2)
                            else:
                                Yo, Uo, Vo, a, ac = ly.prep(f)
                                blend(torch, Y, Yo, a, x, y)
                                blend(torch, U, Uo, ac, x // 2, y // 2)
                                blend(torch, V, Vo, ac, x // 2, y // 2)
                        o = ofree.get()
                        o.copy_(g, non_blocking=True)
                        if stream:  # 待つ間は眠る(stream.synchronize は CPU を空回しして待つ)
                            ev = torch.cuda.Event(blocking=True)
                            ev.record(stream)
                            ev.synchronize()
                        brd.put_back(b)
                        wq.put((o, ofree.put))
                if n % 15 == 0:
                    progress(n)
        wq.put(None)
        wt.join()
        if werr:
            raise RuntimeError(f"エンコーダーに渡せませんでした: {enc.errtext()}")
        enc.inp.close()
        code = enc.p.wait()
        if code:
            raise RuntimeError(f"エンコードに失敗しました: {enc.errtext()}")
        progress(N)
    finally:
        for ly in layers:
            ly.close()
        brd.close()
        if wt.is_alive():
            try:
                wq.put_nowait(None)
            except queue.Full:
                pass
        enc.kill()


class _null:
    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


def serve():
    """書き出し1回ぶんの常駐。区間ごとにスレッドで並列に作る"""
    import torch
    if torch.cuda.is_available():
        torch.zeros(1, device="cuda")  # CUDA を先に起動しておく
    out_lock = threading.Lock()

    def say(s):
        with out_lock:
            sys.stdout.write(s + "\n")
            sys.stdout.flush()

    def one(req):
        k = req["k"]
        try:
            with open(req["job"], encoding="utf-8") as f:
                job = json.load(f)
            run(job, req["out"], lambda n: say(f"{k} frame={n}"), torch)
            say(f"{k} ok")
        except Exception as e:
            say(f"{k} err {type(e).__name__}: {e}".replace("\n", " ")[:1500])

    say("ready")
    ths = []
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        th = threading.Thread(target=one, args=(json.loads(line),), daemon=True)
        th.start()
        ths.append(th)
    for th in ths:
        th.join()


def main():
    if sys.argv[1:] == ["--serve"]:
        return serve()
    job_path, out = sys.argv[1], sys.argv[2]
    with open(job_path, encoding="utf-8") as f:
        job = json.load(f)
    try:
        run(job, out, lambda n: print(f"frame={n}", flush=True))
    except Exception as e:
        print(f"自前エンジンで失敗: {type(e).__name__}: {e}", file=sys.stderr, flush=True)
        sys.exit(1)


if __name__ == "__main__":
    main()
