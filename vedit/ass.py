"""テロップ・トピックバーを ASS 字幕として生成(Photoshop で PNG を作る代わり)。"""


def _bgr(hexcolor):
    h = hexcolor.lstrip("#")
    return f"&H{h[4:6]}{h[2:4]}{h[0:2]}&".upper()


def _alpha(opacity):
    return f"&H{int(round((1 - opacity) * 255)):02X}&"


def _t(sec):
    cs = int(round(sec * 100))
    h, cs = divmod(cs, 360000)
    m, cs = divmod(cs, 6000)
    s, cs = divmod(cs, 100)
    return f"{h}:{m:02d}:{s:02d}.{cs:02d}"


def _esc(text):
    return text.replace("\\", "＼").replace("{", "｛").replace("}", "｝").replace("\n", "\\N")


def _text_width(text, size):
    # 全角=size、半角=size*0.55 の概算(トピックバーの幅決定用)
    return sum(size if ord(ch) > 0x2E80 else size * 0.55 for ch in max(text.split("\n"), key=len))


HEAD = (
    "[Script Info]\nScriptType: v4.00+\nPlayResX: {W}\nPlayResY: {H}\nWrapStyle: 2\nScaledBorderAndShadow: yes\n\n"
    "[V4+ Styles]\nFormat: Name,Fontname,Fontsize,PrimaryColour,SecondaryColour,OutlineColour,BackColour,"
    "Bold,Italic,Underline,StrikeOut,ScaleX,ScaleY,Spacing,Angle,BorderStyle,Outline,Shadow,Alignment,"
    "MarginL,MarginR,MarginV,Encoding\n"
    "Style: Default,Yu Gothic UI,48,&H00FFFFFF,&H00FFFFFF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,0,0,2,0,0,0,1\n\n"
    "[Events]\nFormat: Layer,Start,End,Style,Name,MarginL,MarginR,MarginV,Effect,Text\n"
)


def _anim(it, k, x, y):
    """出入りアニメーションのタグ(位置タグ \\pos / \\move を含む)。x,y は最終位置(px)。"""
    kind, ms = it.get("anim", "none"), int(float(it.get("anim_dur", 0.2)) * 1000)
    if kind == "fade":
        return f"\\fad({ms},{ms})\\pos({x},{y})"
    if kind == "pop":
        return f"\\fad({ms // 2},{ms // 2})\\fscx70\\fscy70\\t(0,{ms},\\fscx100\\fscy100)\\pos({x},{y})"
    if kind == "slide-up":
        return f"\\fad({ms},{ms})\\move({x},{y + round(60 * k)},{x},{y},0,{ms})"
    return f"\\pos({x},{y})"


def build(project, W, H, telops, topics):
    """telops/topics は [(start, end, item)] (カット後の時刻)。ASS 文字列を返す。"""
    k = H / 1080.0
    ev = []
    for a, b, it in telops:
        st = project.style(it.get("style", "telop"))
        if it.get("pos"):  # 画面比率(0〜1)で指定した中央揃えの自由配置
            an, x, y = 5, round(it["pos"][0] * W), round(it["pos"][1] * H)
        else:
            align = st.get("align", "bottom")
            an = {"bottom": 2, "top": 8, "center": 5}.get(align, 2)
            mv = round(st.get("margin_v", 70) * k)
            x, y = W // 2, {"bottom": H - mv, "top": mv, "center": H // 2}.get(align, H - mv)
        base = (f"\\an{an}{_anim(it, k, x, y)}\\fs{round(st['size'] * k)}\\b{int(st['bold'])}"
                f"\\fn{st['font']}\\shad0\\1c{_bgr(st['color'])}")
        text = _esc(it["text"])
        if it.get("_hide_from") is not None:  # typewriter(fx.py): ここから後ろを透明に(並びは全部出した時と同じ)
            hf = it["_hide_from"]
            text = _esc(it["text"][:hf]) + "{\\alpha&HFF&}" + _esc(it["text"][hf:])
        if it.get("_show_only") is not None:  # 文字アニメーション(fx.py): この1文字だけ見せる(並びは全部出した時と同じ)
            i, s = it["_show_only"], it["text"]
            text = "{\\alpha&HFF&}" + _esc(s[:i]) + "{\\alpha&H00&}" + _esc(s[i]) + "{\\alpha&HFF&}" + _esc(s[i + 1:])
        only = it.get("_layers")  # "outline" = 縁だけ / "fill" = 文字だけ(fx.py の文字アニメーション)
        outs = st["outlines"]
        for layer, o in enumerate(outs if only != "fill" else []):
            # 外側の縁ほど太く: 内側の縁との合計幅で描き、上のレイヤーが内側を上書きして二重縁になる
            w = round(sum(x_["width"] for x_ in outs[layer:]) * k, 1)
            ev.append(f"Dialogue: {layer},{_t(a)},{_t(b)},Default,,0,0,0,,{{{base}\\bord{w}\\3c{_bgr(o['color'])}}}{text}")
        if only != "outline":
            ev.append(f"Dialogue: {len(outs)},{_t(a)},{_t(b)},Default,,0,0,0,,{{{base}\\bord0}}{text}")
    for a, b, it in topics:
        st = project.style("topic")
        size = round(st["size"] * k)
        px, py = round(st["pad_x"] * k), round(st["pad_y"] * k)
        x, y = round(st["x"] * k), round(st["y"] * k)
        aw = round(st.get("accent_w", 0) * k)
        w = max(st["min_width"] * k, aw + _text_width(it["text"], size) + px * 2)
        h = size + py * 2
        ms = int(float(it.get("anim_dur", 0.2)) * 1000)
        fx = f"\\fad({ms},{ms})" if it.get("anim", "none") != "none" else ""
        rect = f"m 0 0 l {w:.0f} 0 {w:.0f} {h} 0 {h}"
        ev.append(f"Dialogue: 10,{_t(a)},{_t(b)},Default,,0,0,0,,{{{fx}\\an7\\pos({x},{y})\\p1\\bord0\\shad0"
                  f"\\1c{_bgr(st['bar_color'])}\\1a{_alpha(st['bar_alpha'])}}}{rect}{{\\p0}}")
        if aw:
            acc = f"m 0 0 l {aw} 0 {aw} {h} 0 {h}"
            ev.append(f"Dialogue: 11,{_t(a)},{_t(b)},Default,,0,0,0,,{{{fx}\\an7\\pos({x},{y})\\p1\\bord0\\shad0"
                      f"\\1c{_bgr(st['accent_color'])}}}{acc}{{\\p0}}")
        ev.append(f"Dialogue: 12,{_t(a)},{_t(b)},Default,,0,0,0,,{{{fx}\\an4\\pos({x + aw + px},{y + h / 2:.0f})\\fs{size}"
                  f"\\b{int(st['bold'])}\\fn{st['font']}\\bord0\\shad0\\1c{_bgr(st['color'])}}}{_esc(it['text'])}")
    return HEAD.format(W=W, H=H) + "\n".join(ev) + "\n"
