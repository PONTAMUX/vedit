"""書き出し前の見落としチェック(validate は形式の誤り、こちらは仕上がりの抜け)。
2026-10-10 実作業で実際に起きたもの:
- 画像の頭がカットの中にあると、画像はカットの終わりから出るのに、同じ時刻の効果音はカットと一緒に消える
- 音声設定を変えたあと音声処理をしていないと、プレビューが元の録音(コンプなし)で鳴り「うるさい」と感じる
結果は [{"level": "warn"|"info", "at": 出力秒 or None, "msg": 文}]。"""
import json

from .render import IMG_EXT, Plan
from .timeutil import fmt_time, parse_time

SE_NEAR = 0.25  # 画像の出と効果音の頭がこれ以内なら「鳴っている」


def run(proj, plan=None):
    plan = plan or Plan(proj)
    d, res = proj.data, []
    warn = lambda at, msg, level="warn": res.append({"level": level, "at": None if at is None else round(at, 3), "msg": msg})
    names = d.get("vtracks") or []
    tname = lambda it: (names[t - 2] if 0 <= (t := max(2, round(float(it.get("track") or 2)))) - 2 < len(names) else f"V{t}")
    ovs = plan.conv(plan.overlays())
    ses = [a for a, b, it in plan.sfx() if it.get("kind") != "bgm"]

    # 1) 画像が出るのに効果音が無い(空白に置いた章見出しなどは除く。ドドンの間に出すもの)
    #    「画像」という名前のトラックがあればそこだけ(テロップ・見出しも PNG で置くため)
    #    同じ画像が切れ目なく続く所(分割しただけ)は「出た」ではないので除く
    img_tr = [n for n in names if "画像" in n]
    for a, b, it in ovs:
        if not it.get("file") or (img_tr and tname(it) not in img_tr):
            continue
        if any(i2 is not it and tname(i2) == tname(it) and i2.get("file") == it["file"] and a2 < a and abs(b2 - a) <= 0.1 for a2, b2, i2 in ovs):
            continue
        if it.get("gap_at") is not None or it.get("kind") == "footage" or proj.resolve(it["file"]).suffix.lower() not in IMG_EXT:
            continue
        if not any(abs(s - a) <= SE_NEAR for s in ses):
            moved = abs(plan.tm.out(parse_time(it["start"])) - a) > 0.05 if d["timebase"] == "source" else False
            warn(a, f"画像に効果音がありません [{tname(it)}] {it.get('text') or it['file']}"
                    + ("(頭がカットの中 → 効果音をカットの終わりへ)" if moved else ""))

    # 2) 効果音がカットで丸ごと消える(置いたのに鳴らない)
    if d["timebase"] == "source":
        for it in d.get("sfx") or []:
            if it.get("gap_at") is not None or it.get("kind") == "bgm" or "end" not in it:
                continue
            a, b = parse_time(it["start"]), parse_time(it["end"])
            if b > a and not plan.tm.out_range(a, b):
                warn(None, f"効果音がカットの中にあって鳴りません: 元素材 {fmt_time(a)} {it['file']}")

    # 3) 同じトラックで素材が重なる(上書きされて片方が見えない)
    by = {}
    for a, b, it in ovs:
        by.setdefault(tname(it), []).append((a, b, it))
    for tn, lst in by.items():
        lst.sort(key=lambda x: x[0])
        for (a1, b1, i1), (a2, b2, i2) in zip(lst, lst[1:]):
            if a2 < b1 - 0.05:
                warn(a2, f"同じトラックで重なっています [{tn}] {i1.get('text') or i1['file']} / {i2.get('text') or i2['file']}({b1 - a2:.2f}秒)")

    # 4) 音声処理が今の設定と合っていない(プレビューは元の音・書き出しは古い処理の音になる)
    aj = proj.cache_dir / "audio.json"
    if d["audio"]["chain"]:
        try:
            key = json.loads(json.loads(aj.read_text(encoding="utf-8"))["key"])
            if json.dumps(key[2], sort_keys=True) != json.dumps(d["audio"]["chain"], sort_keys=True):
                warn(None, "音声設定が変わっています。音声処理をやり直してください(今のプレビューは元の録音の音)")
        except FileNotFoundError:
            warn(None, "音声処理がまだです(プレビューは元の録音の音)")
        except Exception:
            pass
    res.sort(key=lambda r: (r["at"] is None, r["at"] or 0))
    return res


def measure(path):
    """書き出したファイルの音: ラウドネス(LUFS)・トゥルーピーク(dBTP)・長さ。ffmpeg ebur128。"""
    import re
    import subprocess
    from . import ff
    r = subprocess.run([ff.find("ffmpeg"), "-hide_banner", "-nostdin", "-nostats", "-i", str(path), "-vn", "-af", "ebur128=peak=true",
                        "-f", "null", "-"], capture_output=True, text=True, encoding="utf-8", errors="replace", creationflags=ff.NOWIN)
    tail = r.stderr[r.stderr.rfind("Summary:"):]
    num = lambda pat: (m := re.search(pat, tail)) and float(m.group(1))
    return {"lufs": num(r"I:\s+(-?[\d.]+) LUFS"), "true_peak": num(r"Peak:\s+(-?[\d.]+|-inf) dBFS"),
            "lra": num(r"LRA:\s+(-?[\d.]+) LU"), "duration": round(ff.probe(path)["duration"], 2)}


def verify_output(proj, path, total):
    """書き出し後の音の検査(-14±1 LUFS・ピーク -1dBTP 以下・長さ)。→ (測定値, 警告リスト)"""
    m, res = measure(path), []
    nz = next((s for s in proj.data["audio"]["chain"] if s.get("type") == "normalize"), {})
    want, ceil = nz.get("lufs"), nz.get("peak_db", -1)
    if m["lufs"] is not None and want is not None and abs(m["lufs"] - want) > 1:
        res.append({"level": "warn", "at": None, "msg": f"音量が目標からずれています: {m['lufs']} LUFS(目標 {want})"})
    if m["true_peak"] is not None and m["true_peak"] > max(-1.0, ceil) + 0.3:
        res.append({"level": "warn", "at": None, "msg": f"ピークが高すぎます: {m['true_peak']} dBTP(割れる恐れ。-1 以下に)"})
    if abs(m["duration"] - total) > 0.5:
        res.append({"level": "warn", "at": None, "msg": f"長さが合いません: {m['duration']}秒(予定 {total:.2f}秒)"})
    return m, res
