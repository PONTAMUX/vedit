"""第5段階 第1部の確認用: Python 版(サーバーの /api/save → Project.save)で、C++ 版のテストと同じ編集をした結果を書き出す。

C++ 版(tests/ui_test.cpp)は EditModel の編集操作(分割・範囲カット・削除・ずらし・元に戻す/やり直す・トラックの非表示)をして保存する。
こちらは「その操作がデータにもたらす結果」を手で(app.js の動きを読んで)書き、ブラウザ画面と同じ経路(JS の JSON.stringify → POST /api/save)で保存する。
2つの保存結果がバイト単位で同じなら、操作の意味と保存の形(キー順・インデント・改行・数値の書き方)が Python 版と一致している。

使い方: python ui_ref.py <素材のあるフォルダ(demo.json など)> <作業フォルダ> <出力フォルダ>
作業フォルダはコピー先(C:/Temp/vui の下)。D: や 案件フォルダ には書かない。
"""
import json
import shutil
import sys
import threading
from http.server import ThreadingHTTPServer
from pathlib import Path
from urllib.request import Request, urlopen

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))  # vedit/ を import できるように
from vedit import server  # noqa: E402
from vedit.project import Project  # noqa: E402


def jsnum(v):
    """JS の JSON.stringify と同じ数の書き方(5.0 は 5)に直す。"""
    if isinstance(v, float) and v.is_integer() and abs(v) < 1e16:
        return int(v)
    if isinstance(v, list):
        return [jsnum(x) for x in v]
    if isinstance(v, dict):
        return {k: jsnum(x) for k, x in v.items()}
    return v


def fix3(x):  # +x.toFixed(3)
    from decimal import ROUND_HALF_UP, Decimal
    return float(Decimal(x).quantize(Decimal("0.001"), rounding=ROUND_HALF_UP))


def post_save(srv, project, mtime):
    body = json.dumps({"project": jsnum(project), "mtime": mtime}).encode()
    req = Request(f"http://127.0.0.1:{srv.server_address[1]}/api/save", data=body,
                  headers={"X-Token": srv.token, "Content-Type": "application/json"})
    with urlopen(req) as r:
        return json.loads(r.read())


def main():
    src, work, out = map(Path, sys.argv[1:4])
    for d in (work, out):
        d.mkdir(parents=True, exist_ok=True)
    for name in ("demo.json", "bg.mp4", "bgm.wav"):
        shutil.copy2(src / name, work / name)
    proj = work / "demo.json"

    srv = ThreadingHTTPServer(("127.0.0.1", 0), server.Handler)
    srv.project_path, srv.token, srv.image_root = str(proj), "tok", None
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        # 1) 何もしないで保存(読み込み時の足りないキーの補い方と、書き方の一致)
        p = Project(proj)
        post_save(srv, p.data, proj.stat().st_mtime)
        shutil.copy2(proj, out / "ref_noop.json")

        # 2) 編集してから保存(C++ 側 ui_test.cpp の run_ops と同じ順)
        p = Project(proj)
        d = p.data
        d["cuts"].append([40, 41.5])                       # 1. イン40〜アウト41.5 を X
        d["splits"] = sorted(d["splits"] + [30])           # 2. 30秒で分割(このあたりに素材なし)
        del d["telops"][1]                                 # 3. テロップ(3〜8秒)を Delete
        d["hide"] = ["v2"]                                 # 4. V2 の目を閉じる(キーは末尾に新しくできる)
        d["cuts"].append([30, 40])                         # 5. V1/A1 の区間[30,40] を Delete(空白を残す)
        d["gaps"].append([41.5, 10])                       #    カットは [30,41.5] につながるので、空白の位置は 41.5
        t = d["telops"][1]                                 # 7. 残ったテロップ(9〜14秒)を 3コマ(0.1秒)右へ
        t["start"], t["end"] = fix3(t["start"] + 0.1), fix3(t["end"] + 0.1)
        post_save(srv, d, proj.stat().st_mtime)
        shutil.copy2(proj, out / "ref_ops.json")
    finally:
        srv.shutdown()
    print("ref OK", out)


if __name__ == "__main__":
    main()
