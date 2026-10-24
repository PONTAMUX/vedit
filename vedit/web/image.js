"use strict";
/* vimg(ブイイメージ)— vedit 内蔵の画像編集。Photoshop の代わり(テロップ画像・差し込み画像を作る/直す)。依存なしの Canvas 2D。
   文書(*.vimg.json)の layers は下→上の順。座標はすべてキャンバスのpx で、x,y はレイヤーの中心。
   ?f=<文書の相対パス> で開く。&export=1 は書き出し専用(CLI の vedit image が Edge headless で使う)。
   単独起動(vedit vimg)ではパスは絶対パス。新規文書はサーバーの一時フォルダに置き、保存時に「名前を付けて保存」する。 */
const $ = (s, r = document) => r.querySelector(s);
const h = (tag, attrs = {}, ...kids) => {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === "class") e.className = v;
    else if (k === "style" && typeof v === "object") Object.assign(e.style, v);
    else if (k.startsWith("on")) e.addEventListener(k.slice(2), v);
    else if (v !== false && v != null) e.setAttribute(k, v === true ? "" : v);
  }
  for (const c of kids.flat()) if (c != null) e.append(c.nodeType ? c : document.createTextNode(c));
  return e;
};
const clamp = (v, a, b) => Math.min(b, Math.max(a, v));
const r1 = v => Math.round(v * 10) / 10;
const clone = o => JSON.parse(JSON.stringify(o));
async function api(path, body) {
  const r = await fetch(path, { method: body === undefined ? "GET" : "POST", headers: { "X-Token": TOKEN, "Content-Type": "application/json" }, body: body === undefined ? undefined : JSON.stringify(body) });
  const j = await r.json().catch(() => ({ error: r.ok ? "サーバーの応答を読めません" : "サーバーエラー (" + r.status + ")" }));
  if (!r.ok || j.error) throw new Error(friendlyErr(j.error || r.status));
  return j;
}
function friendlyErr(msg) {  // Python の生の例外文("[Errno 2] No such file ...: 'C:/x/y.png'")を分かる言葉に
  msg = String(msg ?? "");
  const m = msg.match(/\[(?:Errno|WinError)\s*(\d+)\]([^'"]*?)(?::\s*['"](.+?)['"])?\s*$/);
  if (!m) return msg;
  const name = (m[3] || "").split(/[\\/]+/).filter(Boolean).pop() || "";
  const what = { 2: "ファイルが見つかりません", 3: "フォルダが見つかりません", 5: "アクセスが拒否されました", 13: "アクセスが拒否されました",
    17: "同じ名前のファイルが既にあります", 20: "フォルダではありません", 21: "フォルダです(ファイルを指定してください)", 28: "ディスクの空きが足りません",
    32: "ほかのアプリが使用中です", 22: "ファイル名が正しくありません", 123: "ファイル名が正しくありません" }[+m[1]] || "ファイルを扱えませんでした(" + m[2].trim().replace(/:$/, "") + ")";
  return name ? `${what}: ${name}` : what;
}
const SESSION = Date.now();  // 開いている間に下敷き等が作り直されても古い画像を使わないように
const mediaUrl = p => `/media?p=${encodeURIComponent(p)}&t=${TOKEN}&v=${SESSION}`;
let toastTimer;
function toast(msg, ms = 2600) { const t = $("#toast"); t.textContent = msg; t.hidden = false; clearTimeout(toastTimer); toastTimer = setTimeout(() => t.hidden = true, ms); }

/* ---------------- 状態 ---------------- */
const Q = new URLSearchParams(location.search), EXPORT = Q.get("export") === "1";
let docPath = Q.get("f") || "", pngPath = "", doc = null, sel = null;  // sel = レイヤーの id
let STANDALONE = false, isTemp = false;  // 単独起動か / 保存先が未定(新規)か
let tool = "move", shapeKind = "rect", snapOn = true, spaceDown = false, crop = null, guides = [], editing = null, fonts = [];
let undoS = [], redoS = [], dirty = false, view = { z: 1, x: 0, y: 0 }, mouse = null, lastText = null;
const cv = $("#cv"), ov = $("#ov"), vp = $("#vp");

const DEF = {
  common: { name: "", x: 0, y: 0, w: 100, h: 100, rot: 0, opacity: 1, visible: true, locked: false, export: true, shadow: null },
  // テロップ既定 = 本体と同じ HGP創英角ゴシックUB・白文字・黒縁6
  text: { text: "テキスト", font: "HGP創英角ｺﾞｼｯｸUB", size: 64, color: "#FFFFFF", bold: false, line_height: 1.25, letter_spacing: 0, scale_x: 100, max_w: 0,
          align: "center", valign: "middle", outlines: [{ color: "#000000", width: 6 }], gradient: null, auto: true },
  shape: { shape: "rect", fill: "#3B82F6", stroke: null, radius: 24 },
  image: { src: "", flip: false, adjust: { brightness: 0, contrast: 0, saturation: 0 } },
};
const TEXT_STYLE = ["font", "size", "color", "bold", "line_height", "letter_spacing", "scale_x", "max_w", "align", "outlines", "gradient", "shadow"];
const DEF_SHADOW = { color: "#000000", opacity: 0.6, x: 6, y: 6, blur: 8 };
const DEF_GRAD = { top: "#FFF27A", bottom: "#FF9A1F" };
const ICON = { text: "T", shape: "▭", image: "🖼" };
let idN = 0;
const newId = () => "L" + SESSION.toString(36).slice(-4) + (idN++).toString(36);
function fix(l) {  // 足りない項目を既定値で埋める(AI が書いた最小限の JSON でも開けるように)
  for (const [k, v] of Object.entries({ ...DEF.common, ...(DEF[l.type] || {}) })) if (l[k] === undefined) l[k] = clone(v);
  if (!l.id || doc.layers.some(o => o !== l && o.id === l.id)) l.id = newId();
  if (!l.name) l.name = l.type === "text" ? String(l.text).split("\n")[0].slice(0, 20) || "テキスト" : l.type === "shape" ? "図形" : (l.src || "画像").split("/").pop();
  return l;
}
const selL = () => doc && doc.layers.find(l => l.id === sel) || null;
const W = () => doc.width, H = () => doc.height;

/* ---------------- パス・画像 ---------------- */
const dirOf = p => p.includes("/") ? p.slice(0, p.lastIndexOf("/")) : "";
function joinPath(a, b) {
  const out = [];
  for (const s of (a ? a.split("/") : []).concat(b.replace(/\\/g, "/").split("/"))) {
    if (s === "..") { if (out.length && out[out.length - 1] !== "..") out.pop(); else out.push(s); }
    else if (s && s !== ".") out.push(s);
  }
  return out.join("/");
}
const srcUrl = src => mediaUrl(/^([a-z]:)?[\\/]/i.test(src) ? src : joinPath(dirOf(docPath), src));
const IMG = new Map();
function imgOf(src) {
  const url = srcUrl(src); let im = IMG.get(url);
  if (!im) { im = new Image(); im.onload = redraw; im.onerror = () => { im.bad = true; redraw(); }; im.src = url; IMG.set(url, im); }
  return im;
}
const loadImg = src => new Promise(res => {
  const im = imgOf(src);
  if (im.bad || (im.complete && im.naturalWidth)) return res(im);
  im.addEventListener("load", () => res(im), { once: true }); im.addEventListener("error", () => res(im), { once: true });
});

/* ---------------- 描画(画面とPNG書き出しで共通) ---------------- */
const mctx = document.createElement("canvas").getContext("2d");
const fontOf = l => `${l.bold ? "bold " : ""}${l.size}px "${l.font}", sans-serif`;
const outlinePad = l => (l.outlines || []).reduce((s, o) => s + (+o.width || 0), 0);
function rgba(hex, a = 1) {
  const m = /^#?([0-9a-f]{6})$/i.exec(hex || ""); if (!m) return hex || "#000";
  const n = parseInt(m[1], 16); return `rgba(${n >> 16},${(n >> 8) & 255},${n & 255},${a})`;
}
function measure(l) {
  mctx.font = fontOf(l); mctx.letterSpacing = (+l.letter_spacing || 0) + "px";
  const lines = String(l.text ?? "").split("\n"), raw = lines.map(s => mctx.measureText(s).width);
  // 横比率(Photoshop の horizontalScale と同じ)。max_w>0 なら一番長い行がその幅に収まるまで横だけ縮める
  let sx = (+l.scale_x || 100) / 100; const rw = Math.max(1, ...raw);
  if (+l.max_w > 0 && rw * sx > +l.max_w) sx = +l.max_w / rw;
  const ws = raw.map(w => w * sx), adv = l.size * l.line_height;
  return { lines, ws, sx, tw: Math.max(1, ...ws), th: adv * lines.length, adv, pad: outlinePad(l) };
}
function fitText(l) {  // auto のときは枠を文字に合わせる(中心は動かさない)
  if (l.type !== "text" || !l.auto) return;
  const m = measure(l); l.w = Math.ceil(m.tw + m.pad * 2); l.h = Math.ceil(m.th + m.pad * 2);
}
function drawText(x, l) {
  const m = measure(l);
  x.font = fontOf(l); x.letterSpacing = (+l.letter_spacing || 0) + "px";
  x.textBaseline = "middle"; x.lineJoin = "round"; x.textAlign = l.align || "center";
  const txr = l.align === "left" ? -l.w / 2 + m.pad : l.align === "right" ? l.w / 2 - m.pad : 0;
  const top = l.valign === "top" ? -l.h / 2 + m.pad : l.valign === "bottom" ? l.h / 2 - m.pad - m.th : -m.th / 2;
  const ys = m.lines.map((_, i) => top + m.adv * (i + 0.5));
  if (m.sx !== 1) x.scale(m.sx, 1);  // 横比率: x 方向だけ縮める(縁の左右の太さも比率ぶん細くなる。20字前後の行で 0.9 程度なので見た目の差は小さい)
  const tx = txr / m.sx;
  // 縁は外側→内側。外側ほど「内側の縁との合計幅」で太く描き、内側が上書きして多重縁になる(本体の styles.outlines と同じ)
  const outs = l.outlines || [];
  outs.forEach((o, k) => {
    const w = outs.slice(k).reduce((s, q) => s + (+q.width || 0), 0); if (w <= 0) return;
    x.lineWidth = w * 2; x.strokeStyle = o.color;
    m.lines.forEach((s, i) => x.strokeText(s, tx, ys[i]));
  });
  m.lines.forEach((s, i) => {
    if (l.gradient) {  // 行ごとに上→下のグラデーション
      const g = x.createLinearGradient(0, ys[i] - l.size / 2, 0, ys[i] + l.size / 2);
      g.addColorStop(0, l.gradient.top); g.addColorStop(1, l.gradient.bottom); x.fillStyle = g;
    } else x.fillStyle = l.color;
    x.fillText(s, tx, ys[i]);
  });
}
function drawShape(x, l) {
  x.beginPath();
  if (l.shape === "ellipse") x.ellipse(0, 0, l.w / 2, l.h / 2, 0, 0, Math.PI * 2);
  else if (l.shape === "roundrect") x.roundRect(-l.w / 2, -l.h / 2, l.w, l.h, Math.max(0, Math.min(+l.radius || 0, l.w / 2, l.h / 2)));
  else x.rect(-l.w / 2, -l.h / 2, l.w, l.h);
  if (l.fill) { x.fillStyle = l.fill; x.fill(); }
  if (l.stroke && l.stroke.width > 0) { x.lineWidth = l.stroke.width; x.strokeStyle = l.stroke.color; x.lineJoin = "round"; x.stroke(); }
}
const RAST = new WeakMap();
function rasterOf(l) {  // 文字・図形はいったん別キャンバスに描いてから重ねる(不透明度や影が縁ごとに重ならないように)
  const key = JSON.stringify([l.w, l.h, l.text, l.font, l.size, l.color, l.bold, l.line_height, l.letter_spacing, l.scale_x, l.max_w, l.align, l.valign,
    l.outlines, l.gradient, l.shape, l.fill, l.stroke, l.radius]);
  const c = RAST.get(l); if (c && c.key === key) return c;
  const m = (l.type === "text" ? outlinePad(l) + l.size * 0.2 : ((l.stroke && +l.stroke.width) || 0) / 2) + 4;
  const cw = Math.max(1, Math.ceil(l.w + m * 2)), ch = Math.max(1, Math.ceil(l.h + m * 2));
  const cvs = (c && c.cvs) || document.createElement("canvas"); cvs.width = cw; cvs.height = ch;
  const x = cvs.getContext("2d"); x.translate(cw / 2, ch / 2);
  if (l.type === "text") drawText(x, l); else drawShape(x, l);
  const r = { key, cvs, cw, ch }; RAST.set(l, r); return r;
}
function drawLayer(ctx, l) {
  if (!l.visible || l.opacity <= 0) return;
  fitText(l);
  ctx.save();
  ctx.translate(l.x, l.y); if (l.rot) ctx.rotate(l.rot * Math.PI / 180);
  ctx.globalAlpha = clamp(+l.opacity, 0, 1);
  const sh = l.shadow;
  if (sh) { ctx.shadowColor = rgba(sh.color, sh.opacity ?? 1); ctx.shadowBlur = +sh.blur || 0; ctx.shadowOffsetX = +sh.x || 0; ctx.shadowOffsetY = +sh.y || 0; }
  if (l.type === "image") {
    const im = imgOf(l.src), a = l.adjust || {};
    if (im.complete && im.naturalWidth) {
      if (a.brightness || a.contrast || a.saturation)
        ctx.filter = `brightness(${1 + (+a.brightness || 0) / 100}) contrast(${1 + (+a.contrast || 0) / 100}) saturate(${1 + (+a.saturation || 0) / 100})`;
      if (l.flip) ctx.scale(-1, 1);
      ctx.drawImage(im, -l.w / 2, -l.h / 2, l.w, l.h);
    }
  } else {
    const r = rasterOf(l); ctx.drawImage(r.cvs, -r.cw / 2, -r.ch / 2);
  }
  ctx.restore();
}
function renderTo(ctx, forExport) {
  ctx.clearRect(0, 0, W(), H());
  if (doc.background) { ctx.fillStyle = doc.background; ctx.fillRect(0, 0, W(), H()); }
  for (const l of doc.layers) if (!(forExport && l.export === false)) drawLayer(ctx, l);  // export:false = 下敷き(書き出さない)
}
function exportPNG() {
  const c = document.createElement("canvas"); c.width = W(); c.height = H();
  renderTo(c.getContext("2d"), true);
  return c.toDataURL("image/png");
}

/* ---------------- 画面 ---------------- */
let raf = 0;
const redraw = () => { if (doc && !EXPORT && !raf) raf = requestAnimationFrame(() => { raf = 0; draw(); }); };
function draw() {
  if (cv.width !== W() || cv.height !== H()) { cv.width = W(); cv.height = H(); layoutView(); }
  renderTo(cv.getContext("2d"), false);
  drawOverlay(); if (editing) placeEditor();
}
const toScr = (x, y) => [x * view.z + view.x, y * view.z + view.y];
function toDoc(e) { const r = vp.getBoundingClientRect(); return [(e.clientX - r.left - view.x) / view.z, (e.clientY - r.top - view.y) / view.z]; }
function scrOf(e) { const r = vp.getBoundingClientRect(); return [e.clientX - r.left, e.clientY - r.top]; }
function layoutView() {
  Object.assign(cv.style, { left: view.x + "px", top: view.y + "px", width: W() * view.z + "px", height: H() * view.z + "px" });
  const d = devicePixelRatio || 1;
  if (ov.width !== Math.round(vp.clientWidth * d) || ov.height !== Math.round(vp.clientHeight * d)) { ov.width = Math.round(vp.clientWidth * d); ov.height = Math.round(vp.clientHeight * d); }
  $("#st-zoom").textContent = Math.round(view.z * 100) + "%"; $("#st-size").textContent = `${W()} × ${H()} px`;
  drawOverlay(); if (editing) placeEditor();
}
function fitView() {
  const vw = vp.clientWidth, vh = vp.clientHeight, z = Math.min((vw - 48) / W(), (vh - 48) / H());
  view = { z: Math.max(0.02, z), x: (vw - W() * z) / 2, y: (vh - H() * z) / 2 }; layoutView();
}
function zoomAt(f, sx = vp.clientWidth / 2, sy = vp.clientHeight / 2) {
  const z = clamp(view.z * f, 0.02, 32);
  view.x = sx - (sx - view.x) * z / view.z; view.y = sy - (sy - view.y) * z / view.z; view.z = z; layoutView();
}
// レイヤーのローカル座標(中心原点・回転前)⇔ キャンバス座標
function lpt(l, lx, ly) { const r = (l.rot || 0) * Math.PI / 180, c = Math.cos(r), s = Math.sin(r); return [l.x + lx * c - ly * s, l.y + lx * s + ly * c]; }
function toLocal(l, x, y) { const r = -(l.rot || 0) * Math.PI / 180, c = Math.cos(r), s = Math.sin(r), dx = x - l.x, dy = y - l.y; return [dx * c - dy * s, dx * s + dy * c]; }
function extent(l, rot = l.rot) { const r = (rot || 0) * Math.PI / 180, c = Math.abs(Math.cos(r)), s = Math.abs(Math.sin(r)); return [l.w / 2 * c + l.h / 2 * s, l.w / 2 * s + l.h / 2 * c]; }
const HANDLES = [[-1, -1], [0, -1], [1, -1], [1, 0], [1, 1], [0, 1], [-1, 1], [-1, 0]];
const ROT_GAP = 28;  // 回転ハンドルの位置(画面px、上辺から)
function drawOverlay() {
  const d = devicePixelRatio || 1, x = ov.getContext("2d");
  x.setTransform(d, 0, 0, d, 0, 0); x.clearRect(0, 0, ov.width, ov.height);
  if (!doc) return;
  if (crop) {  // 切り抜き: 範囲外を暗く
    const [ax, ay] = toScr(crop.x, crop.y), cw = crop.w * view.z, chh = crop.h * view.z;
    x.fillStyle = "rgba(0,0,0,.55)"; x.beginPath(); x.rect(0, 0, ov.width / d, ov.height / d); x.rect(ax, ay, cw, chh); x.fill("evenodd");
    x.strokeStyle = "#fff"; x.lineWidth = 1; x.strokeRect(ax + .5, ay + .5, cw, chh);
    x.setLineDash([4, 4]); x.strokeStyle = "rgba(255,255,255,.5)";
    for (const t of [1 / 3, 2 / 3]) { x.beginPath(); x.moveTo(ax + cw * t, ay); x.lineTo(ax + cw * t, ay + chh); x.moveTo(ax, ay + chh * t); x.lineTo(ax + cw, ay + chh * t); x.stroke(); }
    x.setLineDash([]); x.fillStyle = "#fff"; x.font = "12px 'Yu Gothic UI'"; x.fillText(`${Math.round(crop.w)} × ${Math.round(crop.h)}`, ax + 4, ay - 6);
    return;
  }
  const l = selL();
  if (l && l.visible) {
    fitText(l);
    const pts = [[-1, -1], [1, -1], [1, 1], [-1, 1]].map(([a, b]) => toScr(...lpt(l, a * l.w / 2, b * l.h / 2)));
    x.strokeStyle = l.locked ? "#f5a524" : "#3b82f6"; x.lineWidth = 1; x.setLineDash(l.locked ? [5, 4] : []);
    x.beginPath(); pts.forEach((p, i) => i ? x.lineTo(...p) : x.moveTo(...p)); x.closePath(); x.stroke(); x.setLineDash([]);
    if (!l.locked && tool === "move" && !editing) {
      const [tx, ty] = toScr(...lpt(l, 0, -l.h / 2)), [rx, ry] = toScr(...lpt(l, 0, -l.h / 2 - ROT_GAP / view.z));
      x.beginPath(); x.moveTo(tx, ty); x.lineTo(rx, ry); x.stroke();
      x.fillStyle = "#fff"; x.beginPath(); x.arc(rx, ry, 5, 0, Math.PI * 2); x.fill(); x.stroke();
      for (const [a, b] of HANDLES) { const [px, py] = toScr(...lpt(l, a * l.w / 2, b * l.h / 2)); x.fillRect(px - 4, py - 4, 8, 8); x.strokeRect(px - 4 + .5, py - 4 + .5, 7, 7); }
    }
  }
  x.strokeStyle = "#ff3ec8"; x.lineWidth = 1;  // スナップのガイド
  for (const g of guides) {
    x.beginPath();
    if (g.x != null) { const [px] = toScr(g.x, 0); x.moveTo(Math.round(px) + .5, 0); x.lineTo(Math.round(px) + .5, ov.height); }
    else { const [, py] = toScr(0, g.y); x.moveTo(0, Math.round(py) + .5); x.lineTo(ov.width, Math.round(py) + .5); }
    x.stroke();
  }
}
function handleAt(l, sx, sy) {
  if (!l || l.locked || !l.visible) return null;
  const [rx, ry] = toScr(...lpt(l, 0, -l.h / 2 - ROT_GAP / view.z));
  if (Math.hypot(rx - sx, ry - sy) < 9) return { rot: true };
  for (const [a, b] of HANDLES) { const [px, py] = toScr(...lpt(l, a * l.w / 2, b * l.h / 2)); if (Math.abs(px - sx) < 7 && Math.abs(py - sy) < 7) return { hx: a, hy: b }; }
  return null;
}
function hitLayer(x, y) {
  for (let i = doc.layers.length - 1; i >= 0; i--) {
    const l = doc.layers[i]; if (!l.visible || l.locked) continue;
    fitText(l); const [lx, ly] = toLocal(l, x, y);
    if (Math.abs(lx) <= l.w / 2 && Math.abs(ly) <= l.h / 2) return l;
  }
  return null;
}
function cursorFor(l, hd) {
  if (hd.rot) return "crosshair";
  const ang = Math.atan2(hd.hy, hd.hx) * 180 / Math.PI + (l.rot || 0);
  return ["ew-resize", "nwse-resize", "ns-resize", "nesw-resize"][Math.round((((ang % 180) + 180) % 180) / 45) % 4];
}

/* ---------------- 編集・やり直し・保存 ---------------- */
function snap() { undoS.push(JSON.stringify(doc)); if (undoS.length > 100) undoS.shift(); redoS = []; }
function commit(props = true) {
  dirty = true; $("#savestate").textContent = "未保存";
  redraw(); renderLayers(); if (props) renderProps();
}
function doUndo(from, to) {
  if (editing) finishEdit();
  if (!from.length) return;
  const w0 = doc.width, h0 = doc.height;
  to.push(JSON.stringify(doc)); doc = JSON.parse(from.pop()); guides = [];
  if (crop) { crop = null; if (tool === "crop") setTool("move"); }
  if (!selL()) sel = null;
  if (doc.width !== w0 || doc.height !== h0) fitView();  // 切り抜き・サイズ変更を戻したら全体表示し直す
  commit();
}
const undo = () => doUndo(undoS, redoS), redoAct = () => doUndo(redoS, undoS);
async function save() {
  if (isTemp) return saveAs();  // 新規(保存先が未定)は名前を付けて保存
  if (editing) finishEdit();
  if (crop) { toast("切り抜きを確定(Enter)かキャンセル(Esc)してください"); return; }
  $("#savestate").textContent = "保存中…";
  try {
    await Promise.all(doc.layers.filter(l => l.type === "image").map(l => loadImg(l.src)));
    const r = await api("/api/vimg/save", { path: docPath, doc, png: exportPNG() });
    dirty = false; $("#savestate").textContent = "保存済み"; toast("保存しました: " + r.png, 1600);
  } catch (e) { $("#savestate").textContent = "保存失敗: " + e.message; }
}
async function saveAs() {  // 単独起動のみ: 保存ダイアログで場所を選んで .vimg.json と透過PNG を書く
  if (!doc || !STANDALONE) return;
  if (editing) finishEdit();
  if (crop) { toast("切り抜きを確定(Enter)かキャンセル(Esc)してください"); return; }
  $("#savestate").textContent = "保存先を選んでいます…";
  try {
    await Promise.all(doc.layers.filter(l => l.type === "image").map(l => loadImg(l.src)));
    const r = await api("/api/vimg/saveas", { path: docPath, doc, png: exportPNG() });
    if (!r.path) { $("#savestate").textContent = dirty ? "未保存" : (isTemp ? "未保存(新規)" : "保存済み"); return; }
    doc = r.doc; docPath = r.path; pngPath = r.png; isTemp = false;
    undoS = []; redoS = [];  // 画像の参照先が変わるので履歴は捨てる
    dirty = false; $("#savestate").textContent = "保存済み";
    Q.set("f", docPath); Q.delete("new"); history.replaceState(null, "", "?" + Q.toString());
    showName(); renderLayers(); renderProps(); redraw();
    toast("保存しました: " + r.png, 2400);
  } catch (e) { $("#savestate").textContent = "保存失敗: " + e.message; }
}
const discardOk = () => !dirty || confirm("保存していない変更があります。破棄してよいですか?");
function go(path) { dirty = false; location.href = "/image.html?f=" + encodeURIComponent(path); }
async function openFile() {
  if (!discardOk()) return;
  try { const r = await api("/api/vimg/pick_open", {}); if (r.path) go(r.path); }
  catch (e) { toast("開けませんでした: " + e.message, 4000); }
}
const SIZES = [[1920, 1080, "フルHD(動画)"], [1280, 720, "HD"], [3840, 2160, "4K"], [1080, 1920, "縦長(ショート)"], [1080, 1080, "正方形"], [1280, 720, "サムネイル"]];
function startScreen(info, closable) {  // はじめの画面: 新規作成(サイズ選択) / ファイルを開く / 起動フォルダのファイル
  const box = $("#modalbox"); box.className = "start"; box.replaceChildren();
  const wi = h("input", { type: "number", value: 1920, min: 1, max: 16384 }), hi = h("input", { type: "number", value: 1080, min: 1, max: 16384 });
  let bg = null;
  const bgSeg = () => h("span", { class: "seg" }, [[null, "透明"], ["#FFFFFF", "白"], ["#000000", "黒"]].map(([v, t]) =>
    h("button", { class: bg === v ? "on" : "", onclick: e => { bg = v; e.currentTarget.parentNode.replaceWith(bgSeg()); } }, t)));
  const create = async (w, hh) => {
    if (!discardOk()) return;
    try { const r = await api("/api/vimg/blank", { width: +w, height: +hh, background: bg }); go(r.path); }
    catch (e) { toast("作成できませんでした: " + e.message, 4000); }
  };
  box.append(h("h2", {}, "vimg", h("span", { class: "dim" }, "画像編集(ブイイメージ)")),
    h("h4", {}, "新規作成"),
    h("div", { class: "presets" }, SIZES.map(([w, hh, t]) => h("button", { onclick: () => create(w, hh) }, `${w} × ${hh}`, h("small", {}, t)))),
    h("div", { class: "row" }, "サイズを指定", wi, "×", hi, "px", h("span", { class: "sp" }), "背景", bgSeg(),
      h("button", { class: "primary", onclick: () => create(wi.value, hi.value) }, "作成")),
    h("h4", {}, "開く"),
    h("div", { class: "row" }, h("button", { class: "primary", onclick: openFile }, "ファイルを開く…"),
      h("span", { class: "dim" }, ".vimg.json(編集データ)か画像(PNG / JPG / WebP)。画像は隣に .vimg.json を作って開きます")));
  if (info && info.files && info.files.length) {
    box.append(h("h4", {}, "このフォルダ: ", h("span", { class: "dim" }, info.dir)),
      h("div", { class: "files" }, info.files.map(f => h("div", { title: f, onclick: () => { if (discardOk()) go(f); } },
        (/\.vimg\.json$/i.test(f) ? "🎨 " : "🖼 ") + f.split("/").pop()))));
  }
  if (closable) box.append(h("div", { class: "foot" }, h("button", { onclick: () => $("#modal").hidden = true }, "閉じる")));
  $("#modal").hidden = false;
}
function showName() {
  const name = isTemp ? "無題(未保存)" : docPath.split("/").pop().replace(/\.vimg\.json$/i, "");
  document.title = "vimg 画像編集 - " + name; $("#pname").textContent = isTemp ? "無題(保存すると場所を選べます)" : docPath;
}
function select(l) {
  if (editing && editing !== l) finishEdit();
  sel = l ? l.id : null; guides = []; renderLayers(); renderProps(); redraw();
}
function addLayer(l, above = true) {
  snap(); fix(l);
  const cur = selL(), i = cur && above ? doc.layers.indexOf(cur) + 1 : doc.layers.length;
  doc.layers.splice(i, 0, l); sel = l.id; commit();
  return l;
}
function dupLayer() {
  const l = selL(); if (!l) return;
  const c = clone(l); c.id = null; c.name = l.name + " のコピー"; c.locked = false;
  addLayer(c);
}
// レイヤーのコピー&ペースト(アプリ内クリップボード)。将来の複数選択に備えて配列で持つ
// OS のクリップボードにも目印の文字列を入れておき、貼り付け時に「目印のまま = 内部コピーが一番新しい」「画像に変わった = 外で画像をコピーした」と判断する
let layerClip = null;  // { layers: [...JSON], id: 目印, n: 貼り付け回数 }
const selLayers = () => { const l = selL(); return l ? [l] : []; };
function copyLayers() {
  const ls = selLayers(); if (!ls.length) return false;
  if (editing) finishEdit();
  layerClip = { layers: ls.map(clone), id: "vimg-layers:" + SESSION + ":" + Date.now(), n: 0 };
  toast(ls.length > 1 ? `${ls.length} 個のレイヤーをコピーしました` : `コピー: ${ls[0].name}`, 1200);
  return true;
}
function pasteLayers() {
  if (!layerClip) return false;
  layerClip.n++;
  const off = 20 * layerClip.n;  // 貼るたびに 20px ずつずらす
  snap();
  const cur = selL(); let i = cur ? doc.layers.indexOf(cur) + 1 : doc.layers.length, last = null;
  for (const src of layerClip.layers) {
    const c = clone(src); c.id = null; c.locked = false; c.x = r1(c.x + off); c.y = r1(c.y + off);
    fix(c); doc.layers.splice(i++, 0, c); last = c;
  }
  if (last) sel = last.id;
  commit();
  return true;
}
function delLayer() {
  const l = selL(); if (!l) return;
  snap(); const i = doc.layers.indexOf(l); doc.layers.splice(i, 1);
  sel = (doc.layers[Math.min(i, doc.layers.length - 1)] || {}).id || null; commit();
}
function moveLayer(d) {  // d=+1 で上へ(Ctrl+])
  const l = selL(); if (!l) return; const i = doc.layers.indexOf(l), j = clamp(i + d, 0, doc.layers.length - 1); if (i === j) return;
  snap(); doc.layers.splice(i, 1); doc.layers.splice(j, 0, l); commit();
}
function textDefaults() {  // 新しい文字は直前に触った文字の書式を引き継ぐ
  const base = clone(DEF.text);
  if (lastText) for (const k of TEXT_STYLE) if (lastText[k] !== undefined) base[k] = clone(lastText[k]);
  return base;
}
function newText(x, y, text = "テキスト") {
  const l = addLayer({ type: "text", ...textDefaults(), text, x: r1(x), y: r1(y) });
  startEdit(l, true);
}
function newShape(x, y, w = 400, hh = 240) {
  return addLayer({ type: "shape", shape: shapeKind, x: r1(x), y: r1(y), w, h: hh, fill: "#3B82F6", stroke: null, radius: 24 });
}
async function addImage(src, at) {
  const im = await loadImg(src);
  if (im.bad) { toast("画像を読み込めません: " + src, 4000); return; }
  let w = im.naturalWidth, hh = im.naturalHeight; const k = Math.min(1, W() / w, H() / hh); w *= k; hh *= k;  // キャンバスより大きければ収める
  const [x, y] = at || [W() / 2, H() / 2];
  addLayer({ type: "image", src, x: r1(x), y: r1(y), w: r1(w), h: r1(hh), name: src.split("/").pop() });
}
async function addImageFile(file, at) {
  if (!/^image\/(png|jpeg|webp|gif|bmp)$/.test(file.type)) { toast("PNG / JPG / WebP を使ってください"); return; }
  const data = await new Promise(res => { const fr = new FileReader(); fr.onload = () => res(fr.result); fr.readAsDataURL(file); });
  const ext = { "image/png": ".png", "image/jpeg": ".jpg", "image/webp": ".webp", "image/gif": ".gif", "image/bmp": ".bmp" }[file.type];
  const name = file.name && /\.\w+$/.test(file.name) ? file.name : "paste" + ext;
  try { const r = await api("/api/vimg/asset", { doc: docPath, name, data }); await addImage(r.src, at); }
  catch (e) { toast("取り込み失敗: " + e.message, 4000); }
}
async function pickImage() {
  try {
    const p = await api("/api/pick", { kind: "image" }); if (!p.path) return;
    const r = await api("/api/vimg/asset", { doc: docPath, from: p.path }); await addImage(r.src);
  } catch (e) { toast("取り込み失敗: " + e.message, 4000); }
}
function alignLayer(l, ax, ay) {  // キャンバスに対して揃える(回転後の外接矩形で)
  const [ex, ey] = extent(l);
  snap();
  if (ax != null) l.x = r1([ex, W() / 2, W() - ex][ax]);
  if (ay != null) l.y = r1([ey, H() / 2, H() - ey][ay]);
  commit();
}

/* ---------------- 文字の直接編集(ダブルクリック) ---------------- */
const ta = $("#tedit");
function startEdit(l, selectAll) {
  if (l.type !== "text") return;
  if (editing) finishEdit();
  editing = l; sel = l.id; lastText = l;
  ta.hidden = false; ta.value = l.text; ta.dataset.orig = l.text;
  placeEditor(); setTimeout(() => { ta.focus(); if (selectAll) ta.select(); });
  renderProps(); drawOverlay();
}
function placeEditor() {
  const l = editing; if (!l) return;
  fitText(l);
  const [ex, ey] = extent(l), [x0, y0] = toScr(l.x - ex, l.y - ey);
  Object.assign(ta.style, { left: x0 + "px", top: y0 + "px", width: Math.max(40, ex * 2 * view.z) + "px", height: Math.max(20, ey * 2 * view.z) + "px",
    font: `${l.bold ? "bold " : ""}${l.size * view.z}px "${l.font}", sans-serif`, lineHeight: l.line_height, textAlign: l.align || "center",
    letterSpacing: (l.letter_spacing || 0) * view.z + "px", paddingTop: Math.max(0, (ey * 2 - measure(l).th) / 2 * view.z) + "px" });
}
ta.addEventListener("input", () => {
  const l = editing; if (!l) return;
  if (ta.value === ta.dataset.orig) return;
  if (!ta.dataset.snapped) { undoS.push(JSON.stringify({ ...doc, layers: doc.layers.map(o => o === l ? { ...o, text: ta.dataset.orig } : o) })); redoS = []; ta.dataset.snapped = 1; }
  l.text = ta.value; dirty = true; $("#savestate").textContent = "未保存"; redraw(); renderLayers();
  const box = $("#p-text"); if (box) box.value = ta.value;
});
ta.addEventListener("blur", () => finishEdit());
function finishEdit() {
  const l = editing; if (!l) return;
  const changed = !!ta.dataset.snapped;
  editing = null; ta.hidden = true; delete ta.dataset.snapped;
  if (!String(l.text).trim()) { doc.layers.splice(doc.layers.indexOf(l), 1); if (sel === l.id) sel = null; }
  else if (!changed) { redraw(); renderProps(); return; }
  else if (/^テキスト$|^$/.test(l.name) || l.name === String(ta.dataset.orig).split("\n")[0].slice(0, 20)) l.name = String(l.text).split("\n")[0].slice(0, 20);
  commit();
}

/* ---------------- マウス ---------------- */
function drag(onMove, onUp) {
  const mv = ev => onMove(ev), up = ev => { removeEventListener("pointermove", mv); removeEventListener("pointerup", up); onUp && onUp(ev); };
  addEventListener("pointermove", mv); addEventListener("pointerup", up);
}
function dragPan(e) {
  const x0 = e.clientX, y0 = e.clientY, v0 = { ...view }; vp.style.cursor = "grabbing";
  drag(ev => { view.x = v0.x + ev.clientX - x0; view.y = v0.y + ev.clientY - y0; layoutView(); }, () => vp.style.cursor = "");
}
function snapPos(l, nx, ny) {  // 外接矩形の端・中央をキャンバスの端・中央線と他のレイヤーに吸着
  guides = [];
  if (!snapOn) return [nx, ny];
  const [ex, ey] = extent(l), th = 6 / view.z;
  const tx = [0, W() / 2, W()], ty = [0, H() / 2, H()];
  for (const o of doc.layers) if (o !== l && o.visible && o.export !== false) { const [ox, oy] = extent(o); tx.push(o.x - ox, o.x, o.x + ox); ty.push(o.y - oy, o.y, o.y + oy); }
  const best = (pos, e, ts) => { let b = null; for (const t of ts) for (const d of [-e, 0, e]) { const dd = t - (pos + d); if (Math.abs(dd) < th && (!b || Math.abs(dd) < Math.abs(b.d))) b = { d: dd, t }; } return b; };
  const bx = best(nx, ex, tx), by = best(ny, ey, ty);
  if (bx) { nx += bx.d; guides.push({ x: bx.t }); }
  if (by) { ny += by.d; guides.push({ y: by.t }); }
  return [nx, ny];
}
function dragMove(e, l) {
  const [px0, py0] = toDoc(e), sx0 = e.clientX, sy0 = e.clientY, x0 = l.x, y0 = l.y; let moved = false;
  drag(ev => {
    if (!moved) { if (Math.hypot(ev.clientX - sx0, ev.clientY - sy0) < 3) return; snap(); moved = true; }
    const [px, py] = toDoc(ev); let dx = px - px0, dy = py - py0;
    if (ev.shiftKey) { if (Math.abs(dx) > Math.abs(dy)) dy = 0; else dx = 0; }  // Shift = 水平/垂直に限定
    const [nx, ny] = snapPos(l, x0 + dx, y0 + dy); l.x = r1(nx); l.y = r1(ny); redraw();
  }, () => { guides = []; if (moved) commit(); else redraw(); });
}
function dragHandle(e, l, hd) {
  snap();
  const o = { x: l.x, y: l.y, w: l.w, h: l.h, rot: l.rot || 0, size: l.size };
  if (hd.rot) {
    const [px0, py0] = toDoc(e), a0 = Math.atan2(py0 - o.y, px0 - o.x);
    drag(ev => {
      const [px, py] = toDoc(ev); let r = o.rot + (Math.atan2(py - o.y, px - o.x) - a0) * 180 / Math.PI;
      if (ev.shiftKey) r = Math.round(r / 15) * 15;  // Shift = 15°刻み
      r = ((r + 180) % 360 + 360) % 360 - 180; l.rot = r1(r); redraw();
    }, () => commit());
    return;
  }
  const { hx, hy } = hd;
  drag(ev => {
    const [px, py] = toDoc(ev), [lx, ly] = toLocal(o, px, py), alt = ev.altKey;
    const ax = alt ? 0 : -hx * o.w / 2, ay = alt ? 0 : -hy * o.h / 2;  // 動かない側(Alt なら中心)
    let nw = o.w, nh = o.h;
    if (hx) nw = Math.max(2, (lx - ax) * hx * (alt ? 2 : 1));
    if (hy) nh = Math.max(2, (ly - ay) * hy * (alt ? 2 : 1));
    // 角: 画像は既定で比率固定・Shift で自由(Photoshop CC 2019 以降と同じ) / 図形は Shift で比率固定 / 文字は常に比率固定(文字サイズごと拡大縮小)
    const prop = hx && hy && (l.type === "image" ? !ev.shiftKey : ev.shiftKey || l.type === "text");
    if (prop) { const s = Math.max(nw / o.w, nh / o.h); nw = o.w * s; nh = o.h * s; }
    const cx = alt || !hx ? 0 : ax + hx * nw / 2, cy = alt || !hy ? 0 : ay + hy * nh / 2;
    const [nx, ny] = lpt(o, cx, cy);
    l.x = r1(nx); l.y = r1(ny); l.w = r1(nw); l.h = r1(nh);
    if (l.type === "text") { if (prop) l.size = Math.max(1, r1(o.size * nw / o.w)); else l.auto = false; }  // 辺 = 文字枠だけ変える
    redraw();
  }, () => commit());
}
function dragShape(e) {
  const [x0, y0] = toDoc(e), sx0 = e.clientX, sy0 = e.clientY; let l = null;
  drag(ev => {
    if (!l) { if (Math.hypot(ev.clientX - sx0, ev.clientY - sy0) < 4) return; l = newShape(x0, y0, 1, 1); }
    const [px, py] = toDoc(ev); let w = px - x0, hh = py - y0;
    if (ev.shiftKey) { const m = Math.max(Math.abs(w), Math.abs(hh)); w = Math.sign(w || 1) * m; hh = Math.sign(hh || 1) * m; }  // Shift = 正方形/正円
    l.x = r1(x0 + w / 2); l.y = r1(y0 + hh / 2); l.w = r1(Math.max(1, Math.abs(w))); l.h = r1(Math.max(1, Math.abs(hh))); redraw();
  }, () => { if (!l) newShape(x0, y0); else commit(); });
}
function dragCrop(e) {
  const [x0, y0] = toDoc(e);
  drag(ev => {
    let [px, py] = toDoc(ev);
    if (ev.shiftKey) { const m = Math.max(Math.abs(px - x0), Math.abs(py - y0)); px = x0 + Math.sign(px - x0 || 1) * m; py = y0 + Math.sign(py - y0 || 1) * m; }
    crop = { x: Math.round(Math.min(x0, px)), y: Math.round(Math.min(y0, py)), w: Math.round(Math.abs(px - x0)), h: Math.round(Math.abs(py - y0)) };
    drawOverlay(); renderOpts();
  });
}
function applyCrop() {
  if (!crop || crop.w < 2 || crop.h < 2) { toast("ドラッグで範囲を選んでください"); return; }
  snap();
  for (const l of doc.layers) { l.x = r1(l.x - crop.x); l.y = r1(l.y - crop.y); }
  doc.width = crop.w; doc.height = crop.h; crop = null; setTool("move"); fitView(); commit();
}
vp.addEventListener("mousedown", e => { if (e.target !== ta) e.preventDefault(); });  // キャンバスのクリックで文字編集のフォーカスを奪わない
vp.addEventListener("pointerdown", e => {
  if (!doc || e.target === ta) return;
  if (document.activeElement && document.activeElement !== ta) document.activeElement.blur();  // パネルの入力欄から抜ける(ショートカットを効かせる)
  if (editing) { finishEdit(); if (tool === "text") return; }
  if (e.button === 1 || spaceDown || tool === "hand") { e.preventDefault(); return dragPan(e); }
  if (e.button !== 0) return;
  const [x, y] = toDoc(e), [sx, sy] = scrOf(e);
  if (tool === "crop") return dragCrop(e);
  if (tool === "shape") return dragShape(e);
  if (tool === "text") { const l = hitLayer(x, y); if (l && l.type === "text") { select(l); startEdit(l); } else newText(x, y); return; }
  const cur = selL(), hd = handleAt(cur, sx, sy);
  if (hd) return dragHandle(e, cur, hd);
  const l = hitLayer(x, y);
  if (!l) return select(null);
  if (l.id !== sel) select(l);
  if (e.altKey) { dupLayer(); return dragMove(e, selL()); }  // Alt+ドラッグ = 複製して移動(Photoshop と同じ)
  dragMove(e, l);
});
vp.addEventListener("dblclick", e => {
  const [x, y] = toDoc(e), l = hitLayer(x, y);
  if (l && l.type === "text") { select(l); startEdit(l); }
});
vp.addEventListener("pointermove", e => {
  if (!doc) return;
  const [x, y] = toDoc(e); mouse = [x, y]; $("#st-pos").textContent = `x ${Math.round(x)}  y ${Math.round(y)}`;
  if (e.buttons) return;
  let c = { move: "default", text: "text", shape: "crosshair", crop: "crosshair", hand: "grab" }[tool];
  if (spaceDown) c = "grab";
  else if (tool === "move") { const l = selL(), hd = handleAt(l, ...scrOf(e)); if (hd) c = cursorFor(l, hd); else if (hitLayer(x, y)) c = "move"; }
  vp.style.cursor = c;
});
vp.addEventListener("wheel", e => {
  e.preventDefault();
  if (e.altKey || e.ctrlKey) zoomAt(e.deltaY < 0 ? 1.15 : 1 / 1.15, ...scrOf(e));
  else if (e.shiftKey) { view.x -= e.deltaY; layoutView(); }
  else { view.x -= e.deltaX; view.y -= e.deltaY; layoutView(); }
}, { passive: false });
// ドラッグ&ドロップ・貼り付けで画像レイヤー
vp.addEventListener("dragover", e => { if ([...e.dataTransfer.types].includes("Files")) { e.preventDefault(); $("#drop").hidden = false; } });
vp.addEventListener("dragleave", e => { if (e.target === vp || !vp.contains(e.relatedTarget)) $("#drop").hidden = true; });
vp.addEventListener("drop", async e => {
  e.preventDefault(); $("#drop").hidden = true;
  const at = toDoc(e);
  for (const f of e.dataTransfer.files) await addImageFile(f, at);
});
let clipPending = null, focusVal = null;  // focusVal = 文字入力欄にフォーカスした時点の値(編集したかどうかの判定)
addEventListener("focusin", e => { const t = e.target; focusVal = t.isContentEditable ? t.textContent : t.value; });
addEventListener("pointerup", () => {  // ボタン・チェック・スライダーはクリック後にフォーカスを外す(Enter・Space・矢印をキャンバス側のショートカットに戻す)
  const a = document.activeElement;
  if (a && (a.tagName === "BUTTON" || (a.tagName === "INPUT" && /^(range|checkbox|radio|button|submit)$/i.test(a.type)))) setTimeout(() => a.blur(), 0);
});
addEventListener("change", e => { if (e.target.tagName === "SELECT") e.target.blur(); });
const typing = () => { const a = document.activeElement; return !!a && (a === ta || a.isContentEditable || a.tagName === "TEXTAREA" || (a.tagName === "INPUT" && /^(text|number|search|email|url|tel|password)$/i.test(a.type))); };
addEventListener("copy", e => {  // Ctrl+C = 選択レイヤーをコピー(文字入力中・ページの文字を選択中は普通のコピー)
  clipPending = null;
  if (!doc || editing || typing() || String(getSelection() || "")) return;
  if (copyLayers()) { e.preventDefault(); e.clipboardData.setData("text/plain", layerClip.id); }
});
addEventListener("cut", e => {  // Ctrl+X = コピーして削除
  clipPending = null;
  if (!doc || editing || typing() || String(getSelection() || "")) return;
  if (copyLayers()) { e.preventDefault(); e.clipboardData.setData("text/plain", layerClip.id); delLayer(); }
});
addEventListener("paste", e => {
  clipPending = null;
  if (!doc || editing || typing()) return;
  const cd = e.clipboardData, txt = cd ? cd.getData("text/plain") : "";
  const img = cd ? [...cd.items].find(it => it.kind === "file" && it.type.startsWith("image/")) : null;
  if (layerClip && (txt === layerClip.id || !img)) { e.preventDefault(); pasteLayers(); return; }  // 内部コピーの方が新しい(または画像が無い)
  if (img) { e.preventDefault(); addImageFile(img.getAsFile()); }
});

/* ---------------- ツール・キーボード(Photoshop 準拠) ---------------- */
function setTool(t) {
  if (editing) finishEdit();
  if (tool === "crop" && t !== "crop") crop = null;
  tool = t;
  if (t === "crop" && !crop) crop = { x: 0, y: 0, w: W(), h: H() };
  for (const b of document.querySelectorAll("#tools button")) b.classList.toggle("on", b.dataset.tool === t);
  renderOpts(); drawOverlay();
}
const SHAPES = [["rect", "矩形"], ["roundrect", "角丸矩形"], ["ellipse", "楕円"]];
function renderOpts() {
  const o = $("#opts"); o.replaceChildren();
  const hint = t => h("span", { class: "dim" }, t);
  if (tool === "move") o.append(h("button", { class: snapOn ? "on" : "", onclick: () => { snapOn = !snapOn; renderOpts(); } }, "🧲 スナップ"),
    hint("角=拡大縮小(画像は比率固定・Shift で自由 / 図形は Shift で比率固定 / 文字は文字サイズごと) / 辺=枠 / 上の丸=回転(Shift で15°刻み) / Alt=中心から / Alt+ドラッグ=複製 / 矢印=1px(Shift で10px)"));
  else if (tool === "text") o.append(hint("クリックで文字を追加 / 文字をクリックで編集 / Esc・枠の外をクリックで確定"));
  else if (tool === "shape") o.append(h("span", { class: "seg" }, SHAPES.map(([v, t]) => h("button", { class: shapeKind === v ? "on" : "", onclick: () => { shapeKind = v; renderOpts(); } }, t))),
    hint("ドラッグで描く(Shift で正方形・正円) / Shift+U で種類切替"));
  else if (tool === "crop") o.append(h("b", {}, "切り抜き"), h("span", {}, crop ? `${crop.w} × ${crop.h}  (x ${crop.x}, y ${crop.y})` : ""),
    h("button", { class: "primary", onclick: applyCrop }, "適用 (Enter)"), h("button", { onclick: () => setTool("move") }, "キャンセル (Esc)"),
    hint("ドラッグで範囲(Shift で正方形)。キャンバスの外まで広げると余白が増えます"));
  else o.append(hint("ドラッグで表示位置を移動"));
}
document.querySelectorAll("#tools button").forEach(b => b.addEventListener("click", () => setTool(b.dataset.tool)));
addEventListener("keydown", e => {
  if (!doc) return;
  const k = e.key.toLowerCase(), ctrl = e.ctrlKey || e.metaKey, a = document.activeElement;
  if (ctrl && k === "s") { e.preventDefault(); save(); return; }
  if (editing) { if (e.key === "Escape" || (ctrl && e.key === "Enter")) { e.preventDefault(); finishEdit(); } return; }
  if (typing()) {  // 文字入力欄: ふつうの文字入力を優先。値を変えていないときの Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y はアプリの取り消しへ
    if (e.key === "Escape") { a.blur(); return; }
    const hist = ctrl && (k === "z" || k === "y");
    if (!hist || (a.isContentEditable ? a.textContent : a.value) !== focusVal) return;
    a.blur();
  } else if (a && a !== document.body && /INPUT|SELECT|BUTTON/.test(a.tagName) && !ctrl && !e.altKey &&
    /^(arrow\w+|enter| |home|end|pageup|pagedown|tab)$/.test(k)) return;  // スライダー・選択肢などの操作キーはそのまま(それ以外のショートカットは通す)
  if (ctrl && !e.shiftKey && !e.altKey && (k === "c" || k === "x" || k === "v")) {  // copy/cut/paste イベントが来なかったとき(環境による)の保険
    clipPending = k; setTimeout(() => { if (clipPending !== k) return; clipPending = null; if (k === "v") pasteLayers(); else if (copyLayers() && k === "x") delLayer(); }, 60);
    return;
  }
  const l = selL(), act = {
    "ctrl+z": undo, "ctrl+shift+z": redoAct, "ctrl+y": redoAct, "ctrl+j": dupLayer, "ctrl+d": () => select(null),  // Photoshop と同じ: Ctrl+J = 複製 / Ctrl+D = 選択解除
    "ctrl+t": () => { setTool("move"); if (l) toast("変形: 角・辺・上の丸をドラッグ", 1500); },
    "ctrl++": () => zoomAt(1.25), "ctrl+=": () => zoomAt(1.25), "ctrl+;": () => zoomAt(1.25), "ctrl+shift++": () => zoomAt(1.25), "ctrl+shift+;": () => zoomAt(1.25),
    "ctrl+-": () => zoomAt(0.8), "ctrl+0": fitView, "ctrl+1": () => zoomAt(1 / view.z),
    "ctrl+]": () => moveLayer(1), "ctrl+[": () => moveLayer(-1),
    v: () => setTool("move"), t: () => setTool("text"), u: () => setTool("shape"), c: () => setTool("crop"), h: () => setTool("hand"),
    "shift+u": () => { shapeKind = SHAPES[(SHAPES.findIndex(s => s[0] === shapeKind) + 1) % 3][0]; setTool("shape"); },
    delete: delLayer, backspace: delLayer,
    enter: () => { if (tool === "crop") applyCrop(); else if (l && l.type === "text") startEdit(l); },
    escape: () => { if (tool === "crop") setTool("move"); else select(null); },
  }[(ctrl ? "ctrl+" : "") + (e.shiftKey ? "shift+" : "") + k];
  if (act) { e.preventDefault(); act(); return; }
  if (k === " ") { e.preventDefault(); spaceDown = true; vp.style.cursor = "grab"; return; }
  const arrows = { arrowleft: [-1, 0], arrowright: [1, 0], arrowup: [0, -1], arrowdown: [0, 1] }[k];
  if (arrows && l && !l.locked) {
    e.preventDefault(); const n = e.shiftKey ? 10 : 1;
    if (!e.repeat || !undoS.length) snap();
    l.x = r1(l.x + arrows[0] * n); l.y = r1(l.y + arrows[1] * n); commit(false); clearTimeout(nudgeT); nudgeT = setTimeout(renderProps, 300);
  }
});
let nudgeT;
addEventListener("keyup", e => { if (e.key === " ") { spaceDown = false; vp.style.cursor = ""; } });
addEventListener("resize", () => doc && layoutView());
addEventListener("beforeunload", e => { if (dirty) { e.preventDefault(); e.returnValue = ""; } });

/* ---------------- 右パネル: プロパティ ---------------- */
function bind(el, apply, props = false) {
  let started = false;
  el.addEventListener("input", () => { if (!started) { snap(); started = true; } apply(el.type === "checkbox" ? el.checked : el.value); commit(props); });
  el.addEventListener("change", () => started = false);
  el.addEventListener("blur", () => started = false);
  return el;
}
const rowOf = (label, ...kids) => h("div", { class: "row" }, h("label", {}, label), ...kids);
const num = (get, set, o = {}) => bind(h("input", { type: "number", value: r1(get() ?? 0), step: o.step ?? 1, min: o.min, max: o.max, disabled: o.disabled }), v => set(v === "" ? 0 : +v), o.props);
const col = (get, set) => bind(h("input", { type: "color", value: /^#[0-9a-f]{6}$/i.test(get()) ? get() : "#000000" }), set);
const chk = (get, set, props = true) => bind(h("input", { type: "checkbox", checked: !!get() }), set, props);
const rng = (get, set, min, max, step = 1) => bind(h("input", { type: "range", min, max, step, value: get() }), v => set(+v));
const seg = (opts, get, set) => h("span", { class: "seg" }, opts.map(([v, t, tip]) => h("button", { class: get() === v ? "on" : "", title: tip, onclick: () => { snap(); set(v); commit(); } }, t)));
const btn = (t, fn, tip) => h("button", { onclick: fn, title: tip }, t);
function renderProps() {
  const P = $("#props"); if (!P || EXPORT) return;
  P.replaceChildren();
  const l = selL();
  if (!l) {  // キャンバス
    const resize = (w, hh) => { snap(); const dx = (w - W()) / 2, dy = (hh - H()) / 2; for (const o of doc.layers) { o.x = r1(o.x + dx); o.y = r1(o.y + dy); } doc.width = Math.max(1, Math.round(w)); doc.height = Math.max(1, Math.round(hh)); fitView(); commit(); };
    const wi = h("input", { type: "number", value: W(), min: 1 }), hi = h("input", { type: "number", value: H(), min: 1 });
    P.append(h("h4", {}, "キャンバス"),
      rowOf("サイズ", wi, "×", hi, btn("変更", () => resize(+wi.value, +hi.value), "中央基準で広げる/狭める")),
      h("div", { class: "row" }, ...[[1920, 1080], [1280, 720], [1080, 1920], [1080, 1080]].map(([a, b]) => btn(`${a}×${b}`, () => resize(a, b)))),
      rowOf("背景", seg([[null, "透明"], ["c", "単色"]], () => doc.background ? "c" : null, v => doc.background = v ? "#FFFFFF" : null),
        doc.background ? col(() => doc.background, v => doc.background = v) : null),
      h("div", { class: "note" }, "背景を透明にすると、動画に重ねたとき下が透けます。"),
      h("h4", {}, "操作"),
      h("div", { class: "note" }, "V 移動 / T 文字 / U 図形 / C 切り抜き / Ctrl+T 変形 / Ctrl+J 複製 / Delete 削除 / Ctrl+Z・Ctrl+Shift+Z / Ctrl+S 保存(PNGも)"),
      h("div", { class: "note" }, "画像はドラッグ&ドロップか Ctrl+V で追加できます。"));
    return;
  }
  const isT = l.type === "text";
  if (isT) fitText(l);
  P.append(h("h4", {}, { text: "テキスト", shape: "図形", image: "画像" }[l.type] + "レイヤー"),
    rowOf("名前", bind(h("input", { type: "text", value: l.name }), v => l.name = v)),
    rowOf("不透明度", rng(() => Math.round(l.opacity * 100), v => l.opacity = v / 100, 0, 100), num(() => Math.round(l.opacity * 100), v => l.opacity = clamp(v, 0, 100) / 100, { props: false }), "%"),
    rowOf("位置", num(() => l.x, v => l.x = v), num(() => l.y, v => l.y = v)),
    rowOf("大きさ", num(() => l.w, v => { l.w = Math.max(1, v); if (isT) l.auto = false; }, { disabled: isT && l.auto }), num(() => l.h, v => { l.h = Math.max(1, v); if (isT) l.auto = false; }, { disabled: isT && l.auto })),
    rowOf("回転", num(() => l.rot, v => l.rot = v), "°", btn("0°", () => { snap(); l.rot = 0; commit(); })),
    rowOf("揃える", h("span", { class: "seg" }, ["左", "中", "右"].map((t, n) => btn(t, () => alignLayer(l, n, null), "キャンバスの" + t + "に"))),
      h("span", { class: "seg" }, ["上", "中", "下"].map((t, n) => btn(t, () => alignLayer(l, null, n), "キャンバスの" + t + "に")))),
    rowOf("", h("label", { class: "chk" }, chk(() => l.export !== false, v => l.export = v), " 書き出す"), h("label", { class: "chk" }, chk(() => l.locked, v => l.locked = v), " ロック")));
  if (isT) {
    const fontIn = bind(h("input", { type: "text", value: l.font, list: "fontlist" }), v => { l.font = v; lastText = l; });
    fontIn.style.fontFamily = `"${l.font}"`;
    P.append(h("h4", {}, "文字"),
      bind(h("textarea", { id: "p-text", rows: 3 }, l.text), v => l.text = v),
      rowOf("フォント", fontIn),
      rowOf("サイズ", num(() => l.size, v => l.size = Math.max(1, v)), "px", h("label", { class: "chk" }, chk(() => l.bold, v => l.bold = v), " 太字")),
      rowOf("色", col(() => l.color, v => l.color = v), h("label", { class: "chk" }, chk(() => l.gradient, v => l.gradient = v ? clone(DEF_GRAD) : null), " グラデーション")),
      l.gradient ? rowOf("上→下", col(() => l.gradient.top, v => l.gradient.top = v), col(() => l.gradient.bottom, v => l.gradient.bottom = v)) : null,
      rowOf("行間", num(() => l.line_height, v => l.line_height = Math.max(0.5, v), { step: 0.05 }), "倍"),
      rowOf("字間", num(() => l.letter_spacing, v => l.letter_spacing = v), "px"),
      rowOf("横比率", num(() => l.scale_x, v => l.scale_x = Math.max(10, Math.min(400, v))), "%"),
      rowOf("最大幅", num(() => l.max_w, v => l.max_w = Math.max(0, v)), "px(0=なし。超えたら横を縮める)"),
      rowOf("横", seg([["left", "左"], ["center", "中央"], ["right", "右"]], () => l.align, v => l.align = v)),
      rowOf("縦", seg([["top", "上"], ["middle", "中央"], ["bottom", "下"]], () => l.valign, v => l.valign = v), h("label", { class: "chk", title: "オフにすると枠の大きさを辺のハンドルで決められる" }, chk(() => l.auto, v => l.auto = v), " 枠を文字に合わせる")),
      h("h4", {}, "縁取り(外側→内側)"),
      ...l.outlines.map((o, n) => h("div", { class: "row" }, col(() => o.color, v => o.color = v), num(() => o.width, v => o.width = Math.max(0, v), { step: 1, min: 0 }), "px",
        n > 0 ? btn("↑", () => { snap(); l.outlines.splice(n - 1, 0, l.outlines.splice(n, 1)[0]); commit(); }, "外側へ") : null,
        btn("×", () => { snap(); l.outlines.splice(n, 1); commit(); }))),
      h("div", { class: "row" }, btn("+ 縁を追加", () => { snap(); l.outlines.push({ color: "#FFFFFF", width: 4 }); commit(); }),
        btn("テロップ既定に戻す", () => { snap(); Object.assign(l, { font: DEF.text.font, color: "#FFFFFF", outlines: clone(DEF.text.outlines), gradient: null, bold: false }); commit(); })));
  }
  if (l.type === "shape") {
    P.append(h("h4", {}, "図形"),
      rowOf("形", seg(SHAPES, () => l.shape, v => l.shape = v)),
      l.shape === "roundrect" ? rowOf("角の丸み", num(() => l.radius, v => l.radius = Math.max(0, v)), "px") : null,
      rowOf("塗り", h("label", { class: "chk" }, chk(() => l.fill, v => l.fill = v ? "#3B82F6" : null), " あり"), l.fill ? col(() => l.fill, v => l.fill = v) : null),
      rowOf("線", h("label", { class: "chk" }, chk(() => l.stroke, v => l.stroke = v ? { color: "#FFFFFF", width: 6 } : null), " あり"),
        l.stroke ? col(() => l.stroke.color, v => l.stroke.color = v) : null, l.stroke ? num(() => l.stroke.width, v => l.stroke.width = Math.max(0, v)) : null, l.stroke ? "px" : null));
  }
  if (l.type === "image") {
    const a = l.adjust || (l.adjust = clone(DEF.image.adjust));
    P.append(h("h4", {}, "画像"),
      h("div", { class: "note" }, l.src),
      h("div", { class: "row" }, btn("原寸", () => { const im = imgOf(l.src); if (im.naturalWidth) { snap(); l.w = im.naturalWidth; l.h = im.naturalHeight; commit(); } }),
        btn("キャンバスに合わせる", () => { const im = imgOf(l.src); if (!im.naturalWidth) return; snap(); const k = Math.min(W() / im.naturalWidth, H() / im.naturalHeight); l.w = r1(im.naturalWidth * k); l.h = r1(im.naturalHeight * k); l.x = W() / 2; l.y = H() / 2; l.rot = 0; commit(); }),
        btn("左右反転", () => { snap(); l.flip = !l.flip; commit(); })),
      h("h4", {}, "色調整"),
      rowOf("明るさ", rng(() => a.brightness, v => a.brightness = v, -100, 100), h("span", {}, String(a.brightness))),
      rowOf("コントラスト", rng(() => a.contrast, v => a.contrast = v, -100, 100), h("span", {}, String(a.contrast))),
      rowOf("彩度", rng(() => a.saturation, v => a.saturation = v, -100, 100), h("span", {}, String(a.saturation))),
      h("div", { class: "row" }, btn("色調整をリセット", () => { snap(); l.adjust = clone(DEF.image.adjust); commit(); })));
  }
  P.append(h("h4", {}, "ドロップシャドウ"),
    rowOf("", h("label", { class: "chk" }, chk(() => l.shadow, v => l.shadow = v ? clone(DEF_SHADOW) : null), " あり")));
  if (l.shadow) {
    const s = l.shadow;
    P.append(rowOf("色", col(() => s.color, v => s.color = v), num(() => Math.round((s.opacity ?? 1) * 100), v => s.opacity = clamp(v, 0, 100) / 100), "%"),
      rowOf("ずれ", num(() => s.x, v => s.x = v), num(() => s.y, v => s.y = v)),
      rowOf("ぼかし", num(() => s.blur, v => s.blur = Math.max(0, v)), "px"));
  }
}
// 数値表示だけ更新するレンジ(色調整の値の表示)
document.addEventListener("input", e => { if (e.target.type === "range" && e.target.nextSibling && e.target.nextSibling.tagName === "SPAN") e.target.nextSibling.textContent = e.target.value; });

/* ---------------- 右パネル: レイヤー ---------------- */
function renderLayers() {
  const L = $("#layers"); if (!L || EXPORT) return;
  L.replaceChildren();
  let dragId = null;
  for (const l of [...doc.layers].reverse()) {
    const nm = h("span", { class: "nm", title: "ダブルクリックで名前を変更" }, l.name, l.export === false ? h("span", { class: "noexp" }, " 書き出さない") : null);
    const row = h("div", { class: "lay" + (l.id === sel ? " sel" : ""), draggable: "true" },
      h("span", { class: "eye" + (l.visible ? "" : " off"), title: "表示/非表示" }, "👁"),
      h("span", { class: "ic" }, ICON[l.type] || "?"), nm,
      h("span", { class: "lk" + (l.locked ? "" : " off"), title: "ロック(キャンバス上で選択・移動できなくする)" }, "🔒"));
    row.addEventListener("click", e => {
      if (e.target.classList.contains("eye")) { snap(); l.visible = !l.visible; commit(false); return; }
      if (e.target.classList.contains("lk")) { snap(); l.locked = !l.locked; commit(); return; }
      select(l);
    });
    nm.addEventListener("dblclick", () => {
      const inp = h("input", { type: "text", value: l.name });
      const done = () => { if (inp.value && inp.value !== l.name) { snap(); l.name = inp.value; commit(); } else renderLayers(); };
      inp.addEventListener("keydown", e => { if (e.key === "Enter") inp.blur(); if (e.key === "Escape") { inp.value = l.name; inp.blur(); } });
      inp.addEventListener("blur", done);
      nm.replaceChildren(inp); inp.focus(); inp.select();
    });
    // ドラッグで並べ替え(上の行ほど前面)
    row.addEventListener("dragstart", e => { dragId = l.id; e.dataTransfer.effectAllowed = "move"; e.dataTransfer.setData("text/plain", l.id); });
    row.addEventListener("dragover", e => {
      if (!dragId) return; e.preventDefault();
      const r = row.getBoundingClientRect(), before = e.clientY < r.top + r.height / 2;
      row.classList.toggle("drop-before", before); row.classList.toggle("drop-after", !before);
    });
    row.addEventListener("dragleave", () => row.classList.remove("drop-before", "drop-after"));
    row.addEventListener("drop", e => {
      e.preventDefault(); row.classList.remove("drop-before", "drop-after");
      const src = doc.layers.find(o => o.id === dragId); dragId = null; if (!src || src === l) return;
      const before = e.clientY < row.getBoundingClientRect().top + row.getBoundingClientRect().height / 2;
      snap(); doc.layers.splice(doc.layers.indexOf(src), 1);
      const ti = doc.layers.indexOf(l); doc.layers.splice(before ? ti + 1 : ti, 0, src); commit();
    });
    L.append(row);
  }
  if (!doc.layers.length) L.append(h("div", { class: "note", style: { padding: "10px" } }, "レイヤーがありません。T で文字、U で図形、画像はドラッグ&ドロップ。"));
}
$("#l-text").onclick = () => newText(W() / 2, H() / 2);
$("#l-shape").onclick = () => newShape(W() / 2, H() / 2);
$("#l-img").onclick = pickImage;
$("#l-dup").onclick = dupLayer; $("#l-del").onclick = delLayer;
$("#b-undo").onclick = undo; $("#b-redo").onclick = redoAct; $("#b-save").onclick = save;

/* ---------------- 読み込み ---------------- */
async function loadDoc() {
  if (!/\.vimg\.json$/i.test(docPath)) {  // 画像を直接渡されたら文書にしてから開く
    const r = await api("/api/vimg/open", { path: docPath }); docPath = r.path;
    if (r.created) Q.set("new", "1");
  }
  const r = await api("/api/vimg?p=" + encodeURIComponent(docPath));
  doc = r.doc; docPath = r.path; pngPath = r.png; isTemp = !!r.temp;
  doc.version = doc.version || 1; doc.layers = doc.layers || []; if (doc.background === undefined) doc.background = null;
  for (const l of doc.layers) {
    for (const k of l.type === "image" ? ["x", "y", "w", "h"] : ["x", "y"]) if (l[k] === undefined) l[k] = null;  // 省略 = 後で中央/原寸
    fix(l);
    if (l.type === "image" && (l.w == null || l.h == null || l.x == null || l.y == null)) {  // サイズ未定 = 画像の原寸
      const im = await loadImg(l.src);
      if (l.w == null || l.h == null) { l.w = im.naturalWidth || 100; l.h = im.naturalHeight || 100; }
      if (!doc.width || !doc.height) { doc.width = Math.round(l.w); doc.height = Math.round(l.h); }
      if (l.x == null) l.x = (doc.width || l.w) / 2; if (l.y == null) l.y = (doc.height || l.h) / 2;
    }
  }
  doc.width = Math.round(doc.width || 1920); doc.height = Math.round(doc.height || 1080);
  for (const l of doc.layers) { if (l.x == null) l.x = doc.width / 2; if (l.y == null) l.y = doc.height / 2; }
  lastText = [...doc.layers].reverse().find(l => l.type === "text") || null;
}
async function fontsReady() {
  await Promise.all(doc.layers.filter(l => l.type === "text").map(l => document.fonts.load(fontOf(l), l.text || "あ").catch(() => null)));
}
async function runExport() {  // 書き出し専用モード: 描いて PNG をサーバーへ送るだけ
  try {
    await loadDoc();
    const ims = await Promise.all(doc.layers.filter(l => l.type === "image" && l.visible && l.export !== false).map(l => loadImg(l.src).then(im => [l, im])));
    const bad = ims.filter(([, im]) => im.bad).map(([l]) => l.src);
    if (bad.length) throw new Error("画像を読み込めません: " + bad.join(", "));
    await fontsReady();
    await api("/api/vimg/save", { path: docPath, png: exportPNG(), width: W(), height: H() });
    document.title = "done";
  } catch (e) {
    try { await api("/api/vimg/save", { path: docPath, error: String(e.message || e) }); } catch (_) {}
  }
}
async function init() {
  const info = await api("/api/vimg/info").catch(() => ({ standalone: false }));  // 単独起動(vimg)か
  STANDALONE = !!info.standalone;
  if (STANDALONE) {
    $("#filemenu").hidden = false;
    $("#b-new").onclick = () => startScreen(null, !!doc); $("#b-open").onclick = openFile; $("#b-saveas").onclick = saveAs;
    const ping = () => api("/api/ping").catch(() => {}); ping(); setInterval(ping, 15000);  // 窓を閉じたらサーバーが自動終了する
    addEventListener("keydown", e => {
      if (!(e.ctrlKey || e.metaKey)) return;
      const k = e.key.toLowerCase();
      if (k === "n") { e.preventDefault(); startScreen(null, !!doc); }
      else if (k === "o") { e.preventDefault(); openFile(); }
      else if (k === "s" && e.shiftKey) { e.preventDefault(); e.stopImmediatePropagation(); saveAs(); }
    }, true);
    if (!docPath) { $("#savestate").textContent = ""; cv.style.display = "none"; startScreen(info, false); return; }
  }
  try { await loadDoc(); } catch (e) { $("#props").textContent = "開けませんでした: " + e.message; if (STANDALONE) startScreen(info, false); return; }
  showName();
  fonts = await api("/api/fonts").catch(() => []);
  document.body.append(h("datalist", { id: "fontlist" }, fonts.map(f => h("option", { value: f }))));
  await fontsReady();
  cv.width = W(); cv.height = H(); fitView(); setTool("move"); renderLayers(); renderProps(); draw();
  if (isTemp) $("#savestate").textContent = "未保存(新規)";
  if (Q.get("new") === "1" && !isTemp) {  // 本体から新規作成した直後: 初期状態(テロップ等)を PNG にしておく
    Q.delete("new"); history.replaceState(null, "", "?" + Q.toString());
    await save();
  }
}
if (EXPORT) runExport(); else init();
