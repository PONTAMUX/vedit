"""GUI の右上に出す CPU / GPU / メモリの使用率。GPU は nvidia-smi(NVIDIA のみ。無ければ出さない)。
nvidia-smi は呼ぶたびに起動する(常駐させると GUI を閉じた後も残るため)。結果は少しの間使い回す。"""
import shutil
import subprocess
import threading
import time

from . import ff

try:
    import psutil
except ImportError:  # 無くても動く(CPU・メモリは出さない)
    psutil = None

_lock = threading.Lock()
_last = {"t": 0.0, "v": None}
_smi = shutil.which("nvidia-smi")


def _gpu():
    global _smi
    if not _smi:
        return None
    try:
        out = subprocess.run([_smi, "--query-gpu=utilization.gpu,memory.used,memory.total", "--format=csv,noheader,nounits"],
                             capture_output=True, text=True, timeout=3, creationflags=ff.NOWIN).stdout
        u, used, total = [float(x) for x in out.splitlines()[0].split(",")]
        return {"gpu": u, "vram_used": used / 1024, "vram_total": total / 1024}
    except Exception:
        _smi = None  # 失敗したら以後は聞かない
        return None


def stats():
    with _lock:
        if _last["v"] is not None and time.time() - _last["t"] < 1.5:
            return _last["v"]
        v = {}
        if psutil:
            vm = psutil.virtual_memory()
            v.update(cpu=psutil.cpu_percent(interval=None), mem=vm.percent, mem_used=vm.used / 2 ** 30, mem_total=vm.total / 2 ** 30)
        v.update(_gpu() or {})
        _last.update(t=time.time(), v=v)
        return v
