"""Premiere のキーボードショートカット(.kys)を vedit のキーマップ(JSON)に変換する。"""
import json
import os
import xml.etree.ElementTree as ET
from pathlib import Path

# vedit のアクション → 既定キー(Premiere 標準に近い配置)
DEFAULT = {
    "play": ["Space"], "frameBack": ["Left"], "frameFwd": ["Right"], "back5": ["Shift+Left"], "fwd5": ["Shift+Right"],
    "prevEdit": ["Up"], "nextEdit": ["Down"], "home": ["Home"], "end": ["End"],
    "markIn": ["I"], "markOut": ["O"], "gotoIn": ["Shift+I"], "gotoOut": ["Shift+O"],
    "extract": [";"], "cutRange": ["X"], "rippleTrimPrev": ["Q"], "rippleTrimNext": ["W"],
    "split": ["Ctrl+K"], "razorTool": ["C"], "selectTool": ["V"], "cutTool": ["Shift+C"],
    "delete": ["Delete"], "rippleDelete": ["Shift+Delete"],
    "undo": ["Ctrl+Z"], "redo": ["Ctrl+Shift+Z", "Ctrl+Y"], "save": ["Ctrl+S"], "saveAs": ["Ctrl+Shift+S"],
    "zoomOut": ["-"], "zoomIn": ["="], "zoomFit": ["\\"],
    "shuttleBack": ["J"], "shuttleStop": ["K"], "shuttleFwd": ["L"],
    "addTelop": ["T"], "addTopic": ["B"], "mute": ["Shift+M"],
    "snapToggle": ["S"], "nudgeLeft": ["Alt+Left"], "nudgeRight": ["Alt+Right"],
    "nudgeLeft5": ["Alt+Shift+Left"], "nudgeRight5": ["Alt+Shift+Right"],
    "copy": ["Ctrl+C"], "paste": ["Ctrl+V"], "selectAll": ["Ctrl+A"], "deselectAll": ["Ctrl+Shift+A"], "linkToggle": ["Ctrl+L"],
}

# Premiere のコマンド名 → vedit のアクション
CMD = {
    "cmd.transport.toggleplay": "play", "cmd.transport.step.back": "frameBack", "cmd.transport.step.forward": "frameFwd",
    "cmd.transport.step.back.five": "back5", "cmd.transport.step.forward.five": "fwd5",
    "cmd.tlnav.prev.edit": "prevEdit", "cmd.tlnav.next.edit": "nextEdit",
    "cmd.transport.sequence.start": "home", "cmd.transport.sequence.end": "end",
    "cmd.common.setin": "markIn", "cmd.common.setout": "markOut", "cmd.goto.in": "gotoIn", "cmd.goto.out": "gotoOut",
    "cmd.sequence.extract": "extract",
    "cmd.sequence.rippletrimpreviousedittoplayhead": "rippleTrimPrev", "cmd.sequence.rippletrimnextedittoplayhead": "rippleTrimNext",
    "cmd.sequence.razorateditline": "split", "cmd.tools.06razor": "razorTool", "cmd.tools.01pointer": "selectTool",
    "cmd.edit.clear": "delete", "cmd.edit.rippledelete": "rippleDelete",
    "cmd.edit.undo": "undo", "cmd.edit.redo": "redo", "cmd.file.save": "save", "cmd.file.saveas": "saveAs",
    "cmd.zoom.out": "zoomOut", "cmd.zoom.in": "zoomIn", "cmd.tlnav.zoomto.sequence": "zoomFit",
    "cmd.transport.shuttle.left": "shuttleBack", "cmd.transport.shuttle.stop": "shuttleStop", "cmd.transport.shuttle.right": "shuttleFwd",
    "cmd.graphics.add.text": "addTelop",
}
SPECIAL = {1: "Space", 2: "Backspace", 4: "Enter", 35: "Delete", 36: "Home", 37: "End", 38: "PageUp", 39: "PageDown",
           42: "Left", 43: "Right", 44: "Up", 45: "Down", 7: "F1"}


def _key(vk):
    if vk >= 0x80000000:
        return chr(vk - 0x80000000).upper()
    return SPECIAL.get(vk)


def convert(kys_path):
    """.kys を読み、{"action":[keys...]} を返す。.kys に無いアクションは既定キーを使う。"""
    sc = ET.parse(kys_path).getroot().find("shortcuts")
    out = {}
    for ctxname in ("context.global", "context.timeline"):
        ctx = sc.find(ctxname)
        for it in (ctx if ctx is not None else []):
            vk, cmd = it.find("virtualkey"), it.find("commandname")
            if vk is None or cmd is None or cmd.text not in CMD:
                continue
            name = _key(int(vk.text))
            if not name:
                continue
            mods = [m for m, t in (("Ctrl", "ctrl"), ("Alt", "alt"), ("Shift", "shift")) if it.find("modifier." + t).text == "true"]
            if cmd.text == "cmd.edit.clear" and mods:
                continue
            combo = "+".join(mods + [name])
            lst = out.setdefault(CMD[cmd.text], [])
            if combo not in lst:
                lst.append(combo)
    keymap = {a: out.get(a) or ks for a, ks in DEFAULT.items()}
    # Premiere 側で他の操作に割り当て済みのキーは、vedit の独自アクション(既定キー)から外して衝突を避ける
    used = {k for a, ks in out.items() for k in ks}
    for a in ("cutRange", "cutTool", "addTelop", "addTopic", "mute", "snapToggle", "nudgeLeft", "nudgeRight", "nudgeLeft5", "nudgeRight5", "copy", "paste", "selectAll", "deselectAll", "linkToggle"):
        if a not in out:
            keymap[a] = [k for k in DEFAULT[a] if k not in used]
    return keymap


def user_path():
    return Path(os.environ.get("APPDATA", str(Path.home()))) / "vedit" / "keymap.json"


def load():
    p = user_path()
    if p.exists():
        try:
            return {**DEFAULT, **json.loads(p.read_text(encoding="utf-8"))}
        except Exception:
            pass
    return DEFAULT


def find_kys():
    """この PC の最新の Premiere カスタム .kys を探す。"""
    base = Path.home() / "OneDrive" / "ドキュメント" / "Adobe" / "Premiere Pro"
    cands = []
    for root in (base, Path.home() / "Documents" / "Adobe" / "Premiere Pro"):
        if root.exists():
            cands += list(root.glob("*/Profile-*/Win/*.kys"))
    return max(cands, key=lambda p: p.stat().st_mtime) if cands else None
