"use strict";
/* vedit GUI — 依存なしのバニラJS。時刻はすべて「元素材の秒」(カットは赤い帯で表示し、再生時に飛ばす) */
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
const sleep = ms => new Promise(r => setTimeout(r, ms));
const fmtT = (t, cs = true) => { t = cs ? Math.round(Math.max(0, t) * 100) / 100 : Math.max(0, t); const m = Math.floor(t / 60), s = t - m * 60; return `${m}:${(cs ? s.toFixed(2) : Math.floor(s) + "").padStart(cs ? 5 : 2, "0")}`; };
const parseT = v => { if (typeof v === "number") return v; const p = String(v).split(":").map(Number); return p.reduce((a, x) => a * 60 + x, 0); };
const clamp = (v, a, b) => Math.min(b, Math.max(a, v));

async function api(path, body) {
  const r = await fetch(path, { method: body === undefined ? "GET" : "POST", headers: { "X-Token": TOKEN, "Content-Type": "application/json" }, body: body === undefined ? undefined : JSON.stringify(body) });
  const j = await r.json();
  if (!r.ok || j.error) throw new Error(j.error || r.status);
  return j;
}
const mediaUrl = (p, v = "") => `/media?p=${encodeURIComponent(p)}&t=${TOKEN}${v ? "&v=" + v : ""}`;
// 差し込み画像のプレビュー表示用(大きい画像はサーバーが縮めたキャッシュを返す。書き出しは元の画像)
const pvImgUrl = f => mediaUrl(f, fileVer[f]) + "&pv=1";
async function runJob(body, onprog) {
  const { id } = await api("/api/job", body);
  for (;;) {
    await sleep(350);
    const j = await api("/api/job?id=" + id);
    onprog && onprog(j.progress);
    if (j.status === "done") return j.result;
    if (j.status === "error") throw new Error(j.error);
  }
}
let toastTimer;
function toast(msg, ms = 2600) { const t = $("#toast"); t.textContent = msg; t.hidden = false; clearTimeout(toastTimer); toastTimer = setTimeout(() => t.hidden = true, ms); }

/* ---------------- 状態 ---------------- */
let S = null, wave = null;
let sel = null;               // {kind, i}
let tool = "select", segSel = null, gapSel = null, bgmSel = false, trkSel = null, gapHold = null, linked = localStorage.getItem("vedit.linked") !== "0", selSet = new Set(), snapOn = true, shuttle = 0, clip = null, fileVer = {}, rate = 1, cutTool = false, markIn = null, markOut = null;
let zoom = 20, tcur = 0, playing = false, collapsed = true;  // collapsed: カット部分を詰めて表示(Premiere同様)
let undoS = [], redoS = [], dirty = false, saveTimer = null, lastMtime = 0, ver = 0, cutsCache = null;
let leftTab = "media", rightTab = "clip", styleTarget = "telop", audioStale = false, audioVer = 0, fonts = [];
const D = () => S.proj;
const dur = () => S.info ? S.info.duration : 60;
let vid = $("#vid"), vidB = $("#vid2");  // vidB = 控え(次のカットの頭に先に合わせておき、カットの瞬間に入れ替える。Premiere のように継ぎ目で止まらない)
const aud = $("#aud"), bgm = $("#bgm");
const LIST = ["overlays", "topics", "telops", "sfx"];

function normalize(p) {
  p.cuts = p.cuts.map(c => [parseT(c[0]), parseT(c[1])]); p.splits = (p.splits || []).map(parseT); p.sfx ||= [];
  for (const it of p.sfx) { it.start = parseT(it.start); it.in = parseT(it.in ?? 0); if (it.end == null) it.end = it.start + sfxLen(it); }  // end 省略 = 音の長さ全部
  for (const k of LIST) for (const it of p[k]) { it.start = parseT(it.start); it.end = parseT(it.end); }
}
/* 効果音(sfx): {file,start,end,gain_db,in}。時刻は差し込みと同じ元素材の秒。end が「音の長さ全部」と同じなら保存時に省く */
const sfxFull = it => S.sfx_dur && S.sfx_dur[it.file] != null ? S.sfx_dur[it.file] : null;
const sfxLen = it => Math.max(0.05, (sfxFull(it) ?? 1) - (+it.in || 0));
const sfxAuto = it => sfxFull(it) != null && Math.abs(it.end - (it.start + sfxLen(it))) < 2e-3;
function projOut() {  // 保存する内容(効果音の自動の end は省く。手で縮めた end は残す)
  const p = D(); if (!(p.sfx || []).some(sfxAuto)) return p;
  const c = JSON.parse(JSON.stringify(p));
  c.sfx.forEach((it, i) => { if (sfxAuto(p.sfx[i])) delete it.end; });
  return c;
}
const getR = (k, it) => k === "cuts" ? [it[0], it[1]] : [it.start, it.end];
const setR = (k, it, a, b) => { if (k === "cuts") { it[0] = a; it[1] = b; } else { it.start = a; it.end = b; } };

/* ---------------- 読み込み・保存・やり直し ---------------- */
async function load(first) {
  S = await api("/api/state"); S.proj = S.project;
  normalize(D());
  audioStale = calcAudioStale(); updAudioWarn();
  lastMtime = S.mtime; dirty = false; cutsCache = null; ver++;
  if (first) try { locks = new Set(JSON.parse(localStorage.getItem(lockKey()) || "[]")); } catch (e) { locks = new Set(); }
  $("#pname").textContent = S.name;
  document.title = "V-edit - " + S.name;
  const w = $("#warn");
  if (D().timebase !== "source") { w.hidden = false; w.textContent = "このプロジェクトは timebase が output です。GUI は source 基準のみ対応です。"; }
  else if (!S.info) { w.hidden = false; w.textContent = "素材の動画が見つかりません: " + D().source; }
  else w.hidden = true;
  setVidSrc(mediaUrl(S.proxy || D().source, S.proxy ? S.proxy_ver : ""));
  if (S.info && !S.proxy) makeProxy();
  if (S.audio && S.has_processed) { aud.src = mediaUrl(S.audio.file, S.audio.key.length + "" + audioVer + "-" + S.audio_ver); }
  setupBgm();
  if (first) {
    fonts = await api("/api/fonts").catch(() => []);
    zoom = clamp((($("#scroller").clientWidth - 40) / tlen()), 0.5, 300);
    $("#zoom").value = zoomToSlider(zoom);
  }
  loadWave();
  fxRequest();
  renderAll(true);
  pvRequest();
}
/* プレビュー用の軽い映像(プロキシ)。元の動画はキーフレームが数秒おきのことが多く、カットを飛ぶシークで映像が止まるので、
   キーフレームを細かくした 720p をキャッシュに作って再生する(server の proxy ジョブ)。書き出しは元の動画のまま */
function setVidSrc(src) {
  if (vid.dataset.src === src) return;
  const first = !vid.dataset.src, t = tcur, was = playing;
  if (!first && was) pause();
  vid.src = src; vid.dataset.src = src; vidB.src = src; vidB.dataset.at = "";
  if (!first) vid.addEventListener("loadedmetadata", () => { seek(t); if (was) play(); }, { once: true });
}
let proxyJob = null;
function makeProxy() {
  if (proxyJob) return;
  const st = $("#pxstat") || h("span", { id: "pxstat", class: "dim", title: "カットを飛ぶ時に映像が止まらないよう、プレビュー用の軽い映像を作っています(書き出しは元の動画)" });
  $("#tcout").closest(".tcbox").after(st); st.textContent = " プレビュー映像を作成中…";
  proxyJob = runJob({ kind: "proxy" }, p => { st.textContent = ` プレビュー映像を作成中 ${Math.round(p * 100)}%`; })
    .then(path => { S.proxy = path; st.textContent = ""; setVidSrc(mediaUrl(path)); toast("プレビューを軽い映像に切り替えました(カットの所で止まりにくくなります)", 2500); })
    .catch(e => { st.textContent = ""; console.warn("プロキシ:", e); });
}
async function loadWave() { try { wave = await api("/api/waveform"); drawWave(); } catch (e) { wave = null; } }
function snap() { undoS.push(JSON.stringify(D())); if (undoS.length > 200) { undoS.shift(); histBase = "(これより前は消えました)"; } redoS = []; }
// 処理済み音声のチェーン(audio.json の key に入っている)と今のチェーンを比べる
const canon = v => Array.isArray(v) ? "[" + v.map(canon).join(",") + "]" : v && typeof v === "object" ? "{" + Object.keys(v).sort().map(k => JSON.stringify(k) + ":" + canon(v[k])).join(",") + "}" : JSON.stringify(v);
function calcAudioStale() {
  if (!S || !S.audio || !S.has_processed) return false;
  try { return canon(JSON.parse(S.audio.key)[2]) !== canon(D().audio.chain); } catch (e) { return false; }
}
/* 再生バーの警告: 処理済みの音が古い・無いと元の録音(コンプ・音量補正なし)で鳴り、大きい所・小さい所がそのまま出る。
   書き出しと聞こえ方が違うので、再生バーに出して押せばその場で作り直せるようにする(2026-10-10 実作業: 気づかず「うるさい」) */
function updAudioWarn() {
  const b = $("#b-rawwarn"); if (!b || !S) return;
  const raw = audioStale || (!S.has_processed && (D().audio?.chain || []).length > 0);
  b.hidden = !raw || !$("#c-proc").checked;
  b.textContent = audioStale ? "⚠ 元の音で再生中(音声設定が変わった)" : "⚠ 元の音で再生中(未処理)";
}
function audioNoteText() {
  const info = S.audio;
  return audioStale ? "⚠ 設定が変わりました。「音声処理を実行」で反映します(今は元の音で再生)" : info && S.has_processed ? `✔ 処理済み: クリック修復 ${info.clicks_repaired ?? "-"}件 / ゲイン補正 ${info.norm_gain_db}dB${info.lufs_after != null ? ` / ${info.lufs_before}→${info.lufs_after} LUFS` : ""}` : "未処理(元の音のまま再生)";
}
/* 同じトラックの同じ時間に素材を2つ重ねない(Premiere の上書き)。直前の状態(undoS の最後)と比べて時刻・トラックが変わった/増えた
   クリップを「今置いたもの」とし、同じレーンで重なる他のクリップの重なった部分を削る(包まれたら削除、内側に置いたら前後に分割)。 */
const NO_OVERLAP = ["overlays", "telops", "topics", "sfx"];
function resolveOverlaps() {
  const prev = undoS.length ? JSON.parse(undoS[undoS.length - 1]) : null; if (!prev) return;
  const key = (k, it) => laneOf(k, it) + "|" + (+it.start).toFixed(3) + "|" + (+it.end).toFixed(3);
  const p = D(), selIt = sel && p[sel.kind] ? p[sel.kind][sel.i] : null, selKs = [...selSet].map(k => { const [kd, j] = k.split(":"); return [kd, p[kd] && p[kd][+j]]; });
  let changed = false;
  for (const k of NO_OVERLAP) {
    const old = new Set((prev[k] || []).map(it => key(k, it))), list = p[k] || [];
    const fin = it => !isGapIt(it) && isFinite(+it.start) && isFinite(+it.end) && +it.end > +it.start;
    const placed = list.filter(it => fin(it) && !old.has(key(k, it)));
    if (!placed.length) continue;
    const newOnes = new Set(placed), add = [], drop = new Set();
    for (const e of placed) {
      const ln = laneOf(k, e), ea = +e.start, eb = +e.end;
      if (isLocked(ln)) continue;
      for (const o of list) {
        if (newOnes.has(o) || drop.has(o) || !fin(o) || laneOf(k, o) !== ln) continue;
        const oa = +o.start, ob = +o.end; if (ob <= ea + 1e-6 || oa >= eb - 1e-6) continue;
        const media = k === "sfx" || k === "overlays" && !!o.file && !/\.(png|jpe?g|webp|bmp|gif)$/i.test(o.file);
        const tail = () => { const t = JSON.parse(JSON.stringify(o)); t.start = +eb.toFixed(3); if (media) t.in = +((+(o.in || 0)) + inDelta(o, oa, eb)).toFixed(3); return t; };
        if (oa >= ea - 1e-6 && ob <= eb + 1e-6) drop.add(o);                 // 丸ごと覆われた → 削除
        else if (oa < ea && ob > eb) { add.push(tail()); o.end = +ea.toFixed(3); }  // 内側に置いた → 前後に分割
        else if (oa < ea) o.end = +ea.toFixed(3);                             // 後ろが重なる → 終わりを詰める
        else { Object.assign(o, tail()); }                                     // 前が重なる → 始まりを後ろへ
        changed = true;
      }
    }
    if (changed) { p[k] = list.filter(o => !drop.has(o)).concat(add); p[k].sort((a, b) => a.start - b.start); }
  }
  if (!changed) return;
  if (selIt) { const i = p[sel.kind].indexOf(selIt); sel = i >= 0 ? { kind: sel.kind, i } : null; }
  selSet = new Set(selKs.filter(([kd, it]) => it && p[kd].includes(it)).map(([kd, it]) => kd + ":" + p[kd].indexOf(it)));
  toast("重なった素材を詰めました(同じトラックには重ねません)", 1500);
}
function commit(opt = {}) {
  if (!opt.keepOverlap) resolveOverlaps();
  cutsCache = null; ver++; dirty = true;
  audioStale = calcAudioStale(); const an = $("#audio-note"); if (an) { an.textContent = audioNoteText(); an.classList.toggle("warn", audioStale); } updAudioWarn();
  $("#savestate").textContent = "未保存…";
  clearTimeout(saveTimer); saveTimer = setTimeout(save, 500);
  fxRequest();
  renderAll(opt.inspector !== false);
  if (playing && pvMode && !pvOn()) { pause(); play(); }  // 再生中に編集したら、古い試聴用音声(前の並び)をやめて処理済み音声で続ける
  pvRequest();
}
function mutate(fn) { snap(); fn(); commit(); }
const rvSaved = () => { if (!$("#review").hidden) Review.refresh(); if (Review.chan) Review.chan.postMessage({ type: "saved" }); };
async function save() {
  clearTimeout(saveTimer);
  try { const r = await api("/api/save", { project: projOut(), mtime: lastMtime }); lastMtime = r.mtime; dirty = false; $("#savestate").textContent = "保存済み"; rvSaved(); }
  catch (e) {
    if (e.message !== "conflict") { $("#savestate").textContent = "保存失敗: " + e.message; return; }
    if (confirm("ほかのプログラム(CLI/AI など)がプロジェクトを書き換えました。\nOK = この画面の内容で上書き保存\nキャンセル = 外部の変更を読み込む(この画面の未保存の編集は破棄)")) {
      const r = await api("/api/save", { project: projOut(), force: true }); lastMtime = r.mtime; dirty = false; $("#savestate").textContent = "保存済み";
    } else { await load(false); undoS.length = 0; redoS.length = 0; histBase = "外部の変更を取り込み"; renderAll(); toast("外部の変更を読み込みました"); }
  }
}
// 別名で保存(Ctrl+Shift+S): 選んだファイルに今の内容を書き、以後はそちらを編集・自動保存する(元のファイルは残る)
async function saveAs() {
  clearTimeout(saveTimer);
  try {
    const r = await api("/api/saveas", { project: projOut() });
    if (!r.path) { if (dirty) save(); return; }  // キャンセル: 未保存分は元のファイルへ
    await load(false); toast("別名で保存しました: " + r.name, 3500);
  } catch (e) { toast("別名で保存: " + e.message, 5000); if (dirty) save(); }
}
function doUndo(from, to) {
  if (!from.length) return;
  to.push(JSON.stringify(D())); S.proj = JSON.parse(from.pop()); sel = null; commit({ keepOverlap: true });
}
/* ---------------- ヒストリー(Premiere のヒストリーパネル) ----------------
   元に戻すの控え(undoS / redoS)をそのまま並べる。操作名は前後の状態を比べて付ける(操作ごとに名前を持たせない)。
   クリックでその時点へ一度に戻る/進む。戻った先から新しく編集すると、その後ろ(灰色)は消える(元に戻すと同じ) */
let histBase = "開く";  // いちばん上の行: 開いた時 / 外部の変更を取り込んだ時 / 古い分が消えた時
const histCache = new Map();  // 前の状態の文字列 → Map(後の状態の文字列 → 操作名)
const HIST_LISTS = { overlays: "差し込み", telops: "テロップ", topics: "トピック", sfx: "効果音" };
const HIST_KEYS = { cuts: "カット", gaps: "空白", splits: "分割点", vmute: "映像だけ黒", amute: "音声だけ無音", aslip: "音声だけずらす", hide: "トラックの表示", mute: "トラックのミュート",
  vtracks: "ビデオトラック", audio: "音声設定", bgm: "BGM", styles: "スタイル", output: "書き出し設定", transitions: "トランジション", v1_fx: "背景のエフェクト", cut_fx: "継ぎ目の処理", source: "元動画", timebase: "時刻の基準" };
const fname = f => String(f || "").split(/[\\/]/).pop().replace(/\.(png|jpe?g|webp|gif|bmp|mp3|wav|m4a|mp4|mov)$/i, "");
function histItemName(k, it) {
  if (k === "telops" || k === "topics") return `「${String(it.text || fname(it.file) || "").replace(/\n/g, " ").slice(0, 14)}」`;
  if (k === "sfx") return fname(it.file);
  return it.preset ? `${it.preset}${it.text ? "「" + String(it.text).replace(/\n/g, " ").slice(0, 10) + "」" : ""}` : it.kind && !it.file ? it.kind : fname(it.file);
}
function histCat(k, it) { return k === "overlays" && it && /\.(png|jpe?g|webp|gif|bmp)$/i.test(it.file || "") ? "画像" : HIST_LISTS[k]; }
function histDescribe(a, b) {
  const parts = [];
  for (const [k] of Object.entries(HIST_LISTS)) {
    const A = a[k] || [], B = b[k] || [], cs = it => canon(it);
    const bs = new Map(); for (const it of B) { const c = cs(it); bs.set(c, (bs.get(c) || 0) + 1); }
    const rem = []; for (const it of A) { const c = cs(it); if (bs.get(c)) bs.set(c, bs.get(c) - 1); else rem.push(it); }
    const as = new Map(); for (const it of A) { const c = cs(it); as.set(c, (as.get(c) || 0) + 1); }
    const add = []; for (const it of B) { const c = cs(it); if (as.get(c)) as.set(c, as.get(c) - 1); else add.push(it); }
    if (!rem.length && !add.length) continue;
    const one = (verb, it, n) => n === 1 ? `${verb} ${histCat(k, it)} ${histItemName(k, it)}` : `${verb} ${histCat(k, it)} ${n}件`;
    if (!rem.length) { parts.push(one("追加", add[0], add.length)); continue; }
    if (!add.length) { parts.push(one("削除", rem[0], rem.length)); continue; }
    if (rem.length === 1 && add.length === 2 && add.every(x => fname(x.file) === fname(rem[0].file) && x.text === rem[0].text)) { parts.push(one("分割", rem[0], 1)); continue; }
    if (rem.length === add.length) {
      // 前後を始まり順に組にして、どう変わったかを見る(時刻だけ同じ量ずれた=移動、片方の端だけ=トリム、トラックだけ=トラック移動)
      const so = x => [...x].sort((p, q) => (+p.start) - (+q.start)), R = so(rem), N = so(add);
      const kinds = R.map((r, i) => {
        const n = N[i], rest = o => { const c = { ...o }; delete c.start; delete c.end; delete c.in; delete c.track; return canon(c); };
        if (rest(r) !== rest(n)) return "変更";
        const ds = (+n.start) - (+r.start), de = (+n.end) - (+r.end);
        if ((r.track ?? 2) !== (n.track ?? 2)) return Math.abs(ds) < 1e-6 ? "トラック移動" : "移動";
        if (Math.abs(ds - de) < 1e-6 && Math.abs(ds) > 1e-6) return "移動";
        return Math.abs(ds) > 1e-6 || Math.abs(de) > 1e-6 ? "トリム" : "変更";
      });
      const v = kinds.every(x => x === kinds[0]) ? kinds[0] : "変更";
      parts.push(one(v, N[0], N.length)); continue;
    }
    parts.push(`変更 ${HIST_LISTS[k]} ${Math.max(rem.length, add.length)}件`);
  }
  for (const k of Object.keys({ ...a, ...b })) {
    if (HIST_LISTS[k] || canon(a[k]) === canon(b[k])) continue;
    const A = Array.isArray(a[k]) ? a[k] : null, B = Array.isArray(b[k]) ? b[k] : null, nm = HIST_KEYS[k] || k;
    if (k === "cuts" && A && B) {
      const sa = new Set(A.map(canon)), sb = new Set(B.map(canon)), na = B.filter(x => !sa.has(canon(x))).length, nr = A.filter(x => !sb.has(canon(x))).length;
      parts.push(nr === 0 ? `カット${na > 1 ? ` ${na}件` : ""}` : na === 0 ? `カットを戻す${nr > 1 ? ` ${nr}件` : ""}` : "カットを変更(リップル)");
    } else if (k === "splits" && A && B) parts.push(B.length > A.length ? "分割" : B.length < A.length ? "分割点を削除" : "分割点を移動");
    else if (A && B && B.length !== A.length) parts.push(`${nm}を${B.length > A.length ? "追加" : "削除"}`);
    else parts.push(`${nm}を変更`);
  }
  if (!parts.length) return "";
  return parts.length > 2 ? `${parts.slice(0, 2).join(" / ")} ほか${parts.length - 2}件` : parts.join(" / ");
}
function histLabel(pa, pb, keep) {
  let m = histCache.get(pa); if (!m) { m = new Map(); histCache.set(pa, m); }
  keep.add(pa);
  if (!m.has(pb)) { try { m.set(pb, histDescribe(JSON.parse(pa), JSON.parse(pb))); } catch (e) { m.set(pb, "変更"); } }
  return m.get(pb);
}
function histStates() { return [...undoS, JSON.stringify(D()), ...[...redoS].reverse()]; }
function histJump(k) {
  const c = undoS.length; if (k === c) return;
  if (playing) pause();
  let cur = JSON.stringify(D());
  while (undoS.length > k) { redoS.push(cur); cur = undoS.pop(); }
  while (undoS.length < k && redoS.length) { undoS.push(cur); cur = redoS.pop(); }
  S.proj = JSON.parse(cur); sel = null; selSet = new Set(); commit({ keepOverlap: true });
}
function histTab(body) {
  const st = histStates(), c = undoS.length, keep = new Set(), ul = h("ul", { class: "hist" });
  const rowsH = [[0, histBase]];
  for (let i = 1; i < st.length; i++) { const lb = histLabel(st[i - 1], st[i], keep); if (lb) rowsH.push([i, lb]); }  // 何も変わらなかった控え(クリックだけ等)は出さない
  const cur = rowsH.filter(([i]) => i <= c).pop()[0];  // 今の位置が出さない控えなら、その手前の行に印
  rowsH.forEach(([i, lb], n) => ul.append(h("li", { class: "hi" + (i === cur ? " cur" : "") + (i > c ? " fut" : ""), title: i > c ? "クリックでここまでやり直す" : "クリックでここまで戻す", onclick: () => histJump(i) },
    h("span", { class: "hn" }, n ? String(n) : ""), lb)));
  for (const k of [...histCache.keys()]) if (!keep.has(k)) histCache.delete(k);
  body.append(h("div", { class: "row" }, h("b", {}, "ヒストリー"), h("span", { class: "dim" }, ` ${c} / ${st.length - 1}(最大200)`)),
    ul, h("p", { class: "note" }, "クリックでその時点に戻ります(灰色はやり直せる操作。ここから編集すると消えます)。Ctrl+Z / Ctrl+Y と同じ控えです。外部(AI・CLI)の変更を取り込むとここから始まります。"));
  requestAnimationFrame(() => body.querySelector(".hi.cur")?.scrollIntoView({ block: "nearest" }));
}
setInterval(async () => {  // AI や CLI が project.json を直接編集したら取り込む
  if (!S || dirty || playing) return;
  try {
    const r = await api("/api/mtime");
    if (r.mtime !== lastMtime) { await load(false); undoS.length = 0; redoS.length = 0; histBase = "外部の変更を取り込み"; renderAll(); toast("外部の変更を取り込みました(元に戻す履歴はここから)"); }
    else if (r.audio_ver !== S.audio_ver || r.proxy_ver !== S.proxy_ver) { await load(false); renderAll(); }  // CLI で音声処理・プレビュー映像を作り直した
  } catch (e) {}
}, 2000);

/* ---------------- カット計算 ---------------- */
function mergedCuts() {
  if (cutsCache) return cutsCache;
  const cs = D().cuts.map(c => [Math.min(c[0], c[1]), Math.max(c[0], c[1])]).sort((a, b) => a[0] - b[0]), out = [];
  for (const c of cs) { if (out.length && c[0] <= out[out.length - 1][1]) out[out.length - 1][1] = Math.max(out[out.length - 1][1], c[1]); else out.push([...c]); }
  return cutsCache = out;
}
// 二分探索(カットが千個を超えると、描画のたびに頭から順に見るのが重い。Olive Track::NearestBlockBeforeOrAt の考え)
const lowerIdx = (n, ok) => { let lo = 0, hi = n; while (lo < hi) { const m = (lo + hi) >> 1; if (ok(m)) hi = m; else lo = m + 1; } return lo; };  // ok(i) が真になる最初の i
const cutAt = t => { const mc = mergedCuts(), i = lowerIdx(mc.length, j => mc[j][1] > t) ; return i < mc.length && t >= mc[i][0] && t < mc[i][1] ? mc[i] : undefined; };
// 空白(gaps) = 元素材の時刻 at の直前に挿入する黒+無音 [at, 長さ]。カット内(カット開始含む)の at はカットの終わりへ寄せ、同じ at はまとめる
function gapsN() {
  const mc = mergedCuts(); if (mc.gaps) return mc.gaps;
  const m = new Map();
  for (const [at0, d] of D().gaps || []) {
    if (!(d >= 0.02)) continue; let at = clamp(+at0, 0, dur());
    for (const [a, b] of mc) if (a <= at && at < b) at = b;
    at = Math.round(at * 1e4) / 1e4; m.set(at, (m.get(at) || 0) + d);
  }
  return mc.gaps = [...m].sort((x, y) => x[0] - y[0]);
}
const normGaps = () => { cutsCache = null; D().gaps = gapsN().filter(([at]) => at < dur() - 1e-3).map(([at, d]) => [+at.toFixed(3), +d.toFixed(3)]); cutsCache = null; };
// 終わりの時刻の換算: ちょうど t に入る空白は含めない(空白の直前で終わるものを空白の中まで伸ばさない。書き出しの TimeMap.out_end と同じ)
const outEnd = t => outTime(t) - gapsN().reduce((s, [at, d]) => s + (Math.abs(at - t) <= 1e-6 ? d : 0), 0);
// 素材(動画・音)は出力の時間で流れるので、a→b で進む素材の秒数はカットを除いた長さ(空白の中のものは空白内の時刻なのでそのまま)
const inDelta = (it, a, b) => isGapIt(it) ? b - a : outTime(b) - outTime(a);
function outTime(t) {  // 結果は頭から順に足していた時と同じ(足す順番も同じ)
  const mc = mergedCuts();
  if (!mc.pre) { let c = 0; mc.pre = [0]; for (const [a, b] of mc) mc.pre.push(c += b - a); }
  const G = gapsN(); if (!G.pre) { let c = 0; G.pre = [0]; for (const [, d] of G) G.pre.push(c += d); }
  const k = lowerIdx(mc.length, i => t <= mc[i][0]);  // t より前で始まるカットの数
  let c = k ? mc.pre[k - 1] + (Math.min(t, mc[k - 1][1]) - mc[k - 1][0]) : 0;
  const g = lowerIdx(G.length, i => G[i][0] > t + 1e-9);
  for (let i = 0; i < g; i++) c -= G[i][1];  // 空白は数個なので順に引く(引く順番を変えない)
  return t - c;
}
// 出力順の並び: {s,e}=元素材の区間 / {gap:長さ, at} 。o=出力上の開始
function pieces() {
  const mc = mergedCuts(); if (mc.pieces) return mc.pieces;
  const G = gapsN(), out = []; let gi = 0, pos = 0, o = 0;
  const keeps = []; for (const [a, b] of mc) { if (a > pos) keeps.push([pos, Math.min(a, dur())]); pos = Math.max(pos, b); } if (pos < dur()) keeps.push([pos, dur()]);
  const push = x => { x.o = o; o += x.gap ?? x.e - x.s; out.push(x); };
  for (let [a, b] of keeps) {
    while (gi < G.length && G[gi][0] <= a + 1e-6) { push({ gap: G[gi][1], at: G[gi][0] }); gi++; }
    while (gi < G.length && G[gi][0] < b - 1e-6) { push({ s: a, e: G[gi][0] }); a = G[gi][0]; push({ gap: G[gi][1], at: G[gi][0] }); gi++; }
    if (b - a > 1e-4) push({ s: a, e: b });
  }
  while (gi < G.length) { push({ gap: G[gi][1], at: G[gi][0] }); gi++; }
  return mc.pieces = out;
}
// リンク解除した編集を出力上の範囲に: black=映像だけ黒 / silent=元の音を消す所 / slips=動かした音 {a,b,o(出力上の頭)}
function trkOut() {
  const mc = mergedCuts(); if (mc.trk) return mc.trk;
  const p = D(), r = ([a, b]) => [outTime(Math.min(a, b)), outTime(Math.max(a, b))];
  const black = (p.vmute || []).map(r), silent = (p.amute || []).map(r), slips = [];
  for (const sl of p.aslip || []) { const o = outTime(sl.a), L = sl.b - sl.a; silent.push([o, o + L], [o + sl.d, o + sl.d + L]); slips.push({ a: sl.a, b: sl.b, o: Math.max(0, o + sl.d) }); }
  return mc.trk = { black, silent, slips };
}
const inR = (rs, o) => rs.some(([a, b]) => o >= a && o < b);
const aud2 = new Audio(); aud2.preload = "auto";
function updateTracks() {  // 再生中: 黒・無音・動かした音をプレビューに反映
  if (!S) return;
  const pvp = playing && pvMode, o = curOut(), T = trkOut(), a1off = isMuted("a1"), base = a1off || inR(T.silent, o);
  updateSfx(o);
  vid.style.visibility = gapHold || (!playing && gpos) || (pvp && srcAt(o).gap) || inR(T.black, o) || isHidden("v1") ? "hidden" : "";
  vid.muted = useProc() || base; aud.muted = base; pva.muted = a1off ? !pvOn() : inR(T.silent, o);  // A1 ミュート中の試聴用音声は効果音だけで作る(pvPieces)
  const sl = !a1off && playing && T.slips.find(x => o >= x.o && o < x.o + x.b - x.a);
  if (!sl) { if (!aud2.paused) aud2.pause(); return; }
  const src = useProc() ? aud.src : vid.currentSrc || vid.src; if (aud2.src !== src) aud2.src = src;
  const want = sl.a + (o - sl.o); aud2.volume = masterVol(); aud2.playbackRate = rate;
  if (aud2.paused || Math.abs(aud2.currentTime - want) > 0.15) aud2.currentTime = want;
  if (!aud2.metered && window.meterConnect) { meterConnect(aud2); aud2.metered = true; }
  if (aud2.paused) aud2.play().catch(() => {});
}
// 効果音: 試聴用音声(pva)に混ぜ込めない間(作り直し中・処理前の音で再生中)だけ、ここで個別に鳴らす。
// Web Audio で先にデコードしておき、その場で鳴らす(<audio> の読み込み・シーク待ちで、ぽペンのような短い音の頭が飛ばないように)
const sfxBuf = new Map(), sfxLive = new Map();  // file → AudioBuffer(読み込み中は null) / 鳴らし中 "file@at" → {src, g}
const sfxOut = () => typeof VM !== "undefined" && VM.ctx ? [VM.ctx, VM.bus] : actx ? [actx, anl] : [null, null];  // meter.js があればそのメーターに通す
function sfxBuffer(file) {
  const [actx] = sfxOut(); if (!actx) return null;
  if (!sfxBuf.has(file)) {
    sfxBuf.set(file, null);
    fetch(mediaUrl(file)).then(r => r.arrayBuffer()).then(ab => actx.decodeAudioData(ab)).then(b => sfxBuf.set(file, b)).catch(() => sfxBuf.delete(file));
  }
  return sfxBuf.get(file);
}
function sfxStopAll() { for (const L of sfxLive.values()) try { L.src.stop(); } catch (e) {} sfxLive.clear(); }
function updateSfx(o) {
  const want = new Set(), [actx, dest] = sfxOut();
  if (playing && !pvMode && actx) for (const s of pvSfx()) {
    if (o < s.at || o >= s.at + s.len) continue;
    const key = s.file + "@" + s.at, vol = Math.pow(10, s.gain_db / 20) * masterVol();
    let L = sfxLive.get(key);
    if (!L) {
      const buf = sfxBuffer(s.file); if (!buf) continue;
      const off = o - s.at, src = actx.createBufferSource(), g = actx.createGain();
      src.buffer = buf; src.playbackRate.value = rate; g.gain.value = vol; src.connect(g).connect(dest);
      src.start(0, s.in + off, Math.max(0.01, s.len - off));
      sfxLive.set(key, L = { src, g });
    } else L.g.gain.value = vol;
    want.add(key);
  }
  for (const [k, L] of sfxLive) if (!want.has(k)) { try { L.src.stop(); } catch (e) {} sfxLive.delete(k); }
}
const X = t => (collapsed ? outTime(t) : t) * zoom;                  // 元素材の時刻 → 画面x(px)
const srcOf = o => {  // 表示上の秒 → 元素材の時刻(空白の上なら空白の後ろの素材の頭)
  if (!collapsed) return o;
  const ps = pieces(), i = lowerIdx(ps.length, j => o < ps[j].o + (ps[j].gap ?? ps[j].e - ps[j].s));
  if (i >= ps.length) return dur();
  const p = ps[i]; return p.gap != null ? p.at : p.s + Math.max(0, o - p.o);
};
// 出力上の秒 o を含む区間の番号(o より後ろで終わる最初の区間。無ければ pieces().length)。区間は隙間なく並ぶので二分探索で引ける
const pieceIdx = o => { const ps = pieces(); return lowerIdx(ps.length, j => o < ps[j].o + (ps[j].gap ?? ps[j].e - ps[j].s)); };
const gapPieceAt = o => { const p = pieces()[pieceIdx(o)]; return p && p.gap != null && o >= p.o ? p : null; };
const inGap = o => !!gapPieceAt(o);
// 空白の中に置く素材(章の頭のドドン・見出しなど): gap_at = 空白の位置(元素材の時刻)、start/end = 空白の頭からの秒。書き出しの Plan.conv と同じ
const isGapIt = it => !!it && it.gap_at != null;
const gapKey = at0 => { let at = clamp(+at0, 0, dur()); for (const [a, b] of mergedCuts()) if (a <= at && at < b) at = b; return Math.round(at * 1e4) / 1e4; };
function gapOf(it) { const at = gapKey(it.gap_at), p = pieces().find(p => p.gap != null && Math.abs(p.at - at) < 1e-6); return p ? { at: p.at, d: p.gap, o: p.o } : null; }
const gapAtOut = o => { const p = gapPieceAt(o); return p ? { at: p.at, d: p.gap, off: o - p.o } : null; };
// 今の位置(出力上の秒)。空白の中で止めている時は gpos {at,d,off}
let gpos = null;
function curOut() {
  if (playing && pvMode) return pva.currentTime;
  const g = gapHold || gpos; if (!g) return outTime(tcur);
  const p = pieces().find(p => p.gap != null && Math.abs(p.at - g.at) < 1e-6);
  return p ? p.o + (gapHold ? gapHoldPos() : gpos.off) : outTime(tcur);
}
// 画面上の左右(px)。空白の素材は詰め表示なら空白の中、元の並びでは空白の印の位置
function itX(it) {
  if (!isGapIt(it)) return [X(it.start), collapsed ? outEnd(it.end) * zoom : X(it.end)];
  const g = gapOf(it); if (!g) return [X(gapKey(it.gap_at)) - 2, X(gapKey(it.gap_at)) + 2];
  if (!collapsed) return [X(g.at) - 3, X(g.at) + 3];
  return [(g.o + +it.start) * zoom, (g.o + Math.min(+it.end, g.d)) * zoom];
}
const srcItems = (k, p = D()) => p[k].filter(it => !isGapIt(it));
const tlen = () => collapsed ? totalOut() : dur();
const totalOut = () => { const mc = mergedCuts(); return mc.tot ??= totalOut0(); };
const totalOut0 = () => dur() - mergedCuts().reduce((s, [a, b]) => s + Math.min(b, dur()) - Math.max(0, a), 0) + gapsN().reduce((s, g) => s + g[1], 0);

/* ---------------- スタイル ---------------- */
const effStyle = name => ({ ...(S.default_styles[name] || S.default_styles.telop), ...(D().styles[name] || {}) });
const styleNames = () => ["telop", ...Object.keys(D().styles).filter(n => n !== "telop" && n !== "topic")];
function editStyle(name, fn) { if (!D().styles[name]) D().styles[name] = JSON.parse(JSON.stringify(effStyle(name))); fn(D().styles[name]); }

/* ---------------- モニター ---------------- */
let K = 0.5;  // 表示倍率 (ステージ高さ / 1080)
function layoutStage() {
  const wrap = $("#stagewrap"), W = S.info ? S.info.width : 1920, H = S.info ? S.info.height : 1080;
  const aw = wrap.clientWidth - 16, ah = wrap.clientHeight - 16, sc = Math.min(aw / W, ah / H);
  const st = $("#stage"); st.style.width = W * sc + "px"; st.style.height = H * sc + "px"; K = H * sc / 1080;
  tlKey = ""; updateStage();
}
function telopEl(it, st, k, sample) {
  const d = h("div", { class: "tl" }), outs = st.outlines;
  d.style.cssText = `font-family:'${st.font}',sans-serif;font-size:${st.size * k}px;font-weight:${st.bold ? 800 : 400};`;
  outs.forEach((o, l) => {
    const w = outs.slice(l).reduce((s, x) => s + x.width, 0) * k * 2;
    d.append(h("span", { style: { color: st.color, webkitTextStroke: `${w}px ${o.color}` } }, it.text));
  });
  d.append(h("span", { style: { color: st.color } }, it.text));
  if (!sample) {
    if (it.pos) { d.style.left = it.pos[0] * 100 + "%"; d.style.top = it.pos[1] * 100 + "%"; d.style.transform = "translate(-50%,-50%)"; }
    else if (st.align === "top") { d.style.left = "50%"; d.style.top = st.margin_v * k + "px"; d.style.transform = "translateX(-50%)"; }
    else if (st.align === "center") { d.style.left = "50%"; d.style.top = "50%"; d.style.transform = "translate(-50%,-50%)"; }
    else { d.style.left = "50%"; d.style.bottom = st.margin_v * k + "px"; d.style.transform = "translateX(-50%)"; }
  }
  return d;
}
function topicEl(it, st, k) {
  const d = h("div", { class: "topic" }, it.text);
  const hex = st.bar_color, a = st.bar_alpha;
  d.style.cssText = `left:${st.x * k}px;top:${st.y * k}px;min-width:${st.min_width * k}px;padding:${st.pad_y * k}px ${st.pad_x * k}px;border-left:${(st.accent_w || 0) * k}px solid ${st.accent_color || "transparent"};` +
    `font-family:'${st.font}',sans-serif;font-size:${st.size * k}px;font-weight:${st.bold ? 800 : 400};color:${st.color};` +
    `background:rgba(${parseInt(hex.slice(1, 3), 16)},${parseInt(hex.slice(3, 5), 16)},${parseInt(hex.slice(5, 7), 16)},${a});line-height:1.2`;
  return d;
}
let tlKey = "", ovEls = new Map();
// 再生中の差し込み動画を目標の時刻へ寄せる。動き出しの遅れなど少しのずれは再生速度で追いつき、hard 秒より大きくずれた時だけシークする
// (毎回シークで合わせると、そのたびに読み込み待ちで絵が止まる。カット・差し込みの多い所でかくついていた)
function chase(m, want, hard) {
  const d = want - m.currentTime;
  if (Math.abs(d) > hard) { if (!m.seeking) { m.currentTime = want; m.playbackRate = rate; } return; }
  if (m.seeking || m.readyState < 3) return;
  const r = Math.abs(d) < 0.03 ? rate : rate * clamp(1 + d * 2, 0.5, 1.6);
  if (Math.abs(m.playbackRate - r) > 0.005) m.playbackRate = r;
}
function parkAt(m, want, tol) { if (Math.abs(m.currentTime - want) > tol && !m.seeking) m.currentTime = want; if (m.playbackRate !== rate) m.playbackRate = rate; }
const PRELOAD = 8, KEEP_VIDS = 8;  // 差し込み動画: 何秒前から読み込むか / 使い終わった動画を何本取っておくか
function ovEl(id, it, isImg) {
  let o = ovEls.get(id);
  if (!o) {
    const m = h(isImg ? "img" : "video", isImg ? { src: pvImgUrl(it.file), decoding: "async" } : { src: mediaUrl(it.file, fileVer[it.file]) + "&pv=1", muted: true, playsinline: true, preload: "auto" });
    o = h("div", { class: "ov" }, m, h("i", { class: "hd", title: "ドラッグで拡大縮小" })); o.m = m; o.isImg = isImg; ovEls.set(id, o); $("#ovlayer").append(o);
    // 画像は展開し終わるまで出さない(出来てから前の画像と入れ替える。作った瞬間はまだ空で、一瞬消えたように見えるため)
    if (isImg) m.decode().catch(() => {}).then(() => { o.ready = true; if (!playing) updateStage(); });
    o.addEventListener("mousedown", e => startOvDrag(e, o.i, o));
  }
  return o;
}
function updateStage() {
  if (!S) return;
  const t = tcur, p = D(), go = gapAtOut(curOut());  // go = 空白の中(黒画面)。出すのはその空白に置いた素材だけ
  const on = it => go ? isGapIt(it) && Math.abs(gapKey(it.gap_at) - go.at) < 1e-6 && go.off >= +it.start && go.off < +it.end : !isGapIt(it) && t >= it.start && t < it.end;
  // テロップ + トピックバー
  const act = [];
  if (!isHidden("tb")) p.topics.forEach((it, i) => { if (on(it)) act.push(["topics", i]); });
  if (!isHidden("t")) p.telops.forEach((it, i) => { if (on(it)) act.push(["telops", i]); });
  const key = ver + "|" + K + "|" + act.map(a => a.join(":")).join(",") + "|" + (sel ? sel.kind + sel.i : "") + "|" + (fxOk() ? VFX.key.length : "");
  if (key !== tlKey) {
    tlKey = key;
    const layer = $("#tllayer"); layer.replaceChildren();
    for (const [kind, i] of act) {
      const it = p[kind][i];
      const el = kind === "topics" ? topicEl(it, effStyle("topic"), K) : telopEl(it, effStyle(it.style || "telop"), K);
      if (sel && sel.kind === kind && sel.i === i) el.classList.add("sel");
      if (kind === "telops" && fxReplacedT(i)) el.style.opacity = 0;  // 動きを付けたテロップは焼いた絵(fxStage)で出す。選択用に残す
      el.addEventListener("mousedown", e => startMonitorDrag(e, kind, i, el));
      layer.append(el);
    }
  }
  // 差し込み
  const used = new Set(), waiting = new Set(), stage = $("#stage");
  p.overlays.forEach((it, i) => {
    if (!on(it) || isHidden("v" + trackOf(it)) || !it.file) return;  // file の無い生成要素(集中線など)はエフェクトのプレビュー(fxStage)で出す
    const t = go ? go.off : tcur, isImg = /\.(png|jpe?g|webp|bmp|gif)$/i.test(it.file);
    // 画像は「トラック+ファイル」で要素を使い回す。同じ画像が細切れで続く所で毎回作り直すと、4K画像のデコードが走ってかくつく
    const id = (isImg ? "v" + trackOf(it) : i) + "|" + it.file + "|" + (fileVer[it.file] || ""); used.add(id);
    const o = ovEl(id, it, isImg);
    o.i = i; o.trk = trackOf(it);
    o.classList.toggle("sel", !!(sel && sel.kind === "overlays" && sel.i === i)); o.classList.toggle("locked", isLocked("v" + trackOf(it)));
    const full = (it.pos || (it.scale == null ? "full" : "top-right")) === "full", s = o.style, m = 40 * K;
    s.cssText = ""; s.zIndex = trackOf(it);  // 上のトラックほど手前
    if (full) { s.inset = 0; o.m.style.objectFit = it.fit === "cover" ? "cover" : "contain"; o.m.style.height = "100%"; }
    else {
      const pos = it.pos || "top-right"; s.width = (it.scale ?? 0.3) * 100 + "%"; o.m.style.height = "auto";
      if (Array.isArray(pos)) { s.left = pos[0] * K + "px"; s.top = pos[1] * K + "px"; }
      else if (pos === "center") { s.left = "50%"; s.top = "50%"; s.transform = "translate(-50%,-50%)"; }
      else { s[pos.includes("left") ? "left" : "right"] = m + "px"; s[pos.includes("top") ? "top" : "bottom"] = m + "px"; }
    }
    const d = Math.min(it.anim_dur ?? 0.3, (it.end - it.start) / 2);
    const pin = 1 - clamp((t - it.start) / d, 0, 1), pout = clamp((t - (it.end - d)) / d, 0, 1);
    let op = 1, tx = 0, ty = 0; const W = stage.clientWidth, H = stage.clientHeight;
    for (const [kind, q] of [[it.in_anim, pin], [it.out_anim, pout]]) {
      if (kind === "fade") op *= 1 - q;
      else if (kind === "slide-left") tx -= q * (o.offsetLeft + o.offsetWidth);
      else if (kind === "slide-right") tx += q * (W - o.offsetLeft);
      else if (kind === "slide-up") ty += q * (H - o.offsetTop);
      else if (kind === "slide-down") ty -= q * (o.offsetTop + o.offsetHeight);
    }
    s.opacity = fxReplaced(i) ? 0 : op;  // エフェクトで焼いた絵に置き換わる要素は透明に(クリックで選べるように残す)
    if (tx || ty) s.transform = (s.transform || "") + ` translate(${tx}px,${ty}px)`;
    if (!o.isImg) {
      // 書き出しと同じく、カットで消えた所は飛ばして続けて流す(出力上の経過時間で進める)。元時間で進めるとカットの多い所で動画が先に終わって真っ暗になる
      const want = (it.in || 0) + Math.max(0, outTime(t) - outTime(it.start));
      if (playing) chase(o.m, want, 1); else parkAt(o.m, want, 0.04);
      if (playing && o.m.paused) o.m.play().catch(() => {}); else if (!playing && !o.m.paused) o.m.pause();
      if (o.m.readyState < 2) { s.opacity = 0; waiting.add(o.trk); }  // まだ絵が出ない間は透明にして、前の映像を残しておく(黒が見えない)
    } else if (!o.ready) { s.opacity = 0; waiting.add(o.trk); }  // 画像も同じ: 展開が終わるまで前の画像を残す
  });
  // 次に始まる動画は3秒前から読み込んで頭に合わせておく(切り替えの瞬間に黒が見えて止まるのを防ぐ。Premiere と同じく途切れずつながる)
  if (!go) {
    const now = curOut();
    p.overlays.forEach((it, i) => {
      if (!it.file || isGapIt(it) || isHidden("v" + trackOf(it)) || /\.(png|jpe?g|webp|bmp|gif)$/i.test(it.file)) return;
      const st = outTime(it.start) - now; if (st <= 0 || st > PRELOAD) return;
      const id = i + "|" + it.file + "|" + (fileVer[it.file] || ""); if (used.has(id)) return; used.add(id);
      const o = ovEl(id, it, false); o.i = i; o.trk = trackOf(it);
      o.style.cssText = "inset:0;opacity:0;pointer-events:none;z-index:0"; o.m.style.height = "100%";
      parkAt(o.m, it.in || 0, 0.04);
      if (!o.m.paused) o.m.pause();
    });
  }
  const nowMs = performance.now(), keep = [];
  for (const [id, o] of [...ovEls].reverse()) {  // 新しく作ったものから残す
    if (used.has(id)) { o.held = 0; continue; }
    // 同じトラックの次の映像がまだ絵を出せない間だけ、前の映像を最後の絵のまま残す(最長1.5秒)
    if (waiting.has(o.trk) && (o.held ||= nowMs) > nowMs - 1500) { if (!o.isImg && !o.m.paused) o.m.pause(); o.style.zIndex = Math.max(0, o.trk - 0.5); continue; }
    // 使い終わった動画はすぐ捨てずに隠して取っておく(戻して見直す時にすぐ出る。メモリ/VRAM を使って速くする)
    if (!o.isImg && keep.length < KEEP_VIDS) { keep.push(id); o.style.cssText = "inset:0;opacity:0;pointer-events:none;z-index:0"; if (!o.m.paused) o.m.pause(); continue; }
    o.remove(); ovEls.delete(id);
  }
  fxStage(curOut());
  if (!tcEditing) { $("#tc").textContent = fmtT(t); $("#tcout").textContent = fmtT(curOut()); }
  $("#tctot").textContent = " / " + fmtT(totalOut());
}
/* ---------------- エフェクトのプレビュー ----------------
   エフェクト・生成要素(集中線など)・トランジション・背景の揺れは、書き出しと同じものをサーバー(fx.py)が焼いてキャッシュし、
   その絵(透明つき webm / PNG)を出力上の時刻に合わせて重ねる。焼き上がるまでは元の要素をそのまま出す。
   エフェクト関係のキーが無いプロジェクトでは何もしない(サーバーにも頼まない)。 */
const FX_BASIC = new Set([undefined, null, "none", "fade", "slide-left", "slide-right", "slide-up", "slide-down"]);
const FX_TRANS_OLD = new Set([undefined, null, "none", "dip-black", "dip-white"]);  // cut_fx.video のうち今までどおり書き出し側で描くもの
const TEL_BASIC = new Set([undefined, null, "none", "fade", "pop", "slide-up"]);
const FX_NEW_KEYS = ["keys", "mask", "matte", "parent", "preset"];  // 付けるとサーバーで焼く要素のキー(fx.NEW_KEYS とプリセット)
const fxNewKey = it => FX_NEW_KEYS.some(k => it[k]) || (it.blend != null && it.blend !== "normal");
const telNeeds = t => !!(t.fx && t.fx.length) || (typeof t.anim === "object" && t.anim !== null) || !TEL_BASIC.has(t.anim) ||
  [t.in_anim, t.out_anim].some(a => a != null && a !== "none") || fxNewKey(t) || !!t.animator;  // fx.needs_bake_telop と同じ
const fxNeeds = it => !it.file ? !!(it.kind || it.preset) : !!(it.fx && it.fx.length) || fxNewKey(it) || [it.in_anim, it.out_anim].some(a => typeof a === "object" && a !== null || !FX_BASIC.has(a));
const sfxMatch = it => typeof it.gain_db === "string";  // "match:ぽペン" の音量はサーバーが計算して VFX.se で返す
function usesFx(p) {
  const cv = (p.cut_fx || {}).video, cvn = cv && typeof cv === "object" ? cv.type : cv;
  return !!((p.v1_fx || []).length || (p.transitions || []).length || !FX_TRANS_OLD.has(cvn) || p.overlays.some(fxNeeds) || p.telops.some(telNeeds) || (p.sfx || []).some(sfxMatch));
}
let VFX = null, fxWant = "", fxTimer = null, fxCur = "";
const fxEls = new Map();
const fxKeyOf = p => JSON.stringify([p.overlays, p.telops.filter(telNeeds), p.styles, p.v1_fx, p.transitions, p.cut_fx, p.cuts, p.gaps, p.hide, p.mute, p.timebase, (p.sfx || []).filter(sfxMatch)]);
function fxRequest() {
  clearTimeout(fxTimer);
  if (!S || !S.info) return;
  const p = D();
  if (!usesFx(p)) { fxCur = ""; if (VFX) { VFX = null; cutsCache = null; updateStage(); } return; }
  fxCur = fxKeyOf(p);
  if (VFX && VFX.key === fxCur) return;
  fxTimer = setTimeout(async () => {
    const key = fxCur; if (key === fxWant) return;
    fxWant = key;
    try {
      const r = await runJob({ kind: "fx", project: JSON.parse(JSON.stringify(D())) });
      if (key !== fxCur) return;  // 焼いている間にまた変わった(次の依頼が出ている)
      VFX = { ...r, key, repl: new Set(r.replaced), replT: new Set(r.replaced_t || []) }; cutsCache = null; tlKey = "";  // 効果音(トランジションの SE)も試聴用音声に入れ直す
      updateStage(); pvRequest();
    } catch (e) { console.warn("エフェクト:", e); }
    finally { if (fxWant === key) fxWant = ""; }
  }, 600);
}
const fxOk = () => !!VFX && VFX.key === fxCur;
const fxReplaced = i => fxOk() && VFX.repl.has(i);
const fxReplacedT = i => fxOk() && VFX.replT.has(i);
function fxStage(now) {
  const used = new Set();
  const vids = [vid, vidB].filter(Boolean);
  let tf = "";
  if (fxOk()) {
    for (const it of VFX.items) {
      if (now < it.a - 2 || now >= it.b) continue;  // 2秒前から読み込んでおく
      const id = it.file + "@" + it.a; used.add(id);
      let e = fxEls.get(id);
      if (!e) {
        e = h(it.img ? "img" : "video", it.img ? { src: mediaUrl(it.file), decoding: "async" } : { src: mediaUrl(it.file), muted: true, playsinline: true, preload: "auto" });
        e.style.cssText = "position:absolute;inset:0;width:100%;height:100%;pointer-events:none;object-fit:contain";
        fxEls.set(id, e); $("#ovlayer").append(e);
      }
      const show = now >= it.a;
      e.style.zIndex = it.track; e.style.opacity = show ? 1 : 0;
      if (!it.img) {
        const want = show ? now - it.a : 0;
        if (playing && show) chase(e, want, 0.6); else parkAt(e, want, 0.02);
        if (playing && show && e.paused) e.play().catch(() => {}); else if ((!playing || !show) && !e.paused) e.pause();
      }
    }
    for (const m of VFX.v1 || []) {  // 背景の揺れ(書き出しと同じ表)
      if (now < m.a || now >= m.b) continue;
      const i = Math.min(m.dx.length - 1, Math.floor((now - m.a) * m.fps + 1e-6)), k = $("#stage").clientHeight / m.H;
      tf = `translate(${m.dx[i] * k}px,${m.dy[i] * k}px) scale(${m.s})`;
      break;
    }
  }
  for (const v of vids) if (v.style.transform !== tf) v.style.transform = tf;
  for (const [id, e] of fxEls) if (!used.has(id)) { if (e.pause) e.pause(); e.remove(); fxEls.delete(id); }
}
/* 時刻の表示: クリックで入力(「1:23.5」「83.5」「1:02:03」)→ Enter でジャンプ / Esc で取り消し。⧉ でコピー */
let tcEditing = false;
function tcEdit(id) {
  if (!S || tcEditing) return;
  const span = $("#" + id), inp = h("input", { class: "tcin", value: span.textContent, spellcheck: "false" });
  tcEditing = true; span.replaceWith(inp); inp.select();
  const done = ok => {
    if (!tcEditing) return; tcEditing = false; inp.replaceWith(span);
    if (ok) {
      const v = inp.value.trim().replace(/[：]/g, ":").replace(/[０-９．]/g, c => String.fromCharCode(c.charCodeAt(0) - 0xFEE0));
      const x = parseT(v);
      if (!v || !isFinite(x)) toast("時刻は 1:23.5 や 83.5 の形で入力してください", 1800);
      else { if (playing) pause(); seek(id === "tcout" ? srcAt(clamp(x, 0, totalOut())).t : x); const sc = $("#scroller"); sc.scrollLeft = Math.max(0, X(tcur) - sc.clientWidth / 2); }
    }
    updateStage();
  };
  inp.addEventListener("keydown", e => { e.stopPropagation(); if (e.key === "Enter") done(true); else if (e.key === "Escape") done(false); });
  inp.addEventListener("blur", () => done(false));
}
async function tcCopy(id, btn) {
  const txt = $("#" + id) ? $("#" + id).textContent : fmtT(id === "tcout" ? outTime(tcur) : tcur);
  try { await navigator.clipboard.writeText(txt); }
  catch (e) { const ta = h("textarea", {}, txt); document.body.append(ta); ta.select(); document.execCommand("copy"); ta.remove(); }
  btn.textContent = "✓"; setTimeout(() => btn.textContent = "⧉", 900); toast("コピーしました: " + txt, 1200);
}
for (const id of ["tc", "tcout"]) $("#" + id).addEventListener("click", () => tcEdit(id));
for (const b of document.querySelectorAll(".tccp")) b.addEventListener("click", () => tcCopy(b.dataset.for, b));
// プログラムモニター上の差し込みを直接操作(Premiere のモーション相当)。位置は 1080p 基準 px の pos、大きさは scale(画面幅比)
function startOvDrag(e, i, o) {
  if (e.button !== 0) return; e.preventDefault(); e.stopPropagation();
  const vl = "v" + trackOf(D().overlays[i]); if (isLocked(vl)) { toast(vl.toUpperCase() + " はロック中です", 1200); return; }
  select("overlays", i);
  const it = D().overlays[i], st = $("#stage").getBoundingClientRect(), r = o.getBoundingClientRect(), k = st.height / 1080;
  const resize = e.target.classList.contains("hd"), x0 = e.clientX, y0 = e.clientY;
  let moved = false;
  const mv = ev => {
    if (!moved) {
      if (Math.abs(ev.clientX - x0) + Math.abs(ev.clientY - y0) < 3) return;
      snap(); moved = true;
      if ((it.pos || (it.scale == null ? "full" : "top-right")) === "full" || !Array.isArray(it.pos)) {  // 全画面/定位置 → 自由配置に切り替え
        it.scale = +(r.width / st.width).toFixed(4); it.pos = [Math.round((r.left - st.left) / k), Math.round((r.top - st.top) / k)];
      }
    }
    if (resize) it.scale = +clamp((r.width + ev.clientX - x0) / st.width, 0.02, 4).toFixed(4);
    else {
      let x = r.left - st.left + ev.clientX - x0, y = r.top - st.top + ev.clientY - y0;
      if (snapOn) {  // 画面中央・端に吸着
        const cx = x + r.width / 2, cy = y + r.height / 2, th = 8;
        if (Math.abs(cx - st.width / 2) < th) x = st.width / 2 - r.width / 2; if (Math.abs(cy - st.height / 2) < th) y = st.height / 2 - r.height / 2;
        if (Math.abs(x) < th) x = 0; if (Math.abs(y) < th) y = 0;
        if (Math.abs(x + r.width - st.width) < th) x = st.width - r.width; if (Math.abs(y + r.height - st.height) < th) y = st.height - r.height;
      }
      it.pos = [Math.round(x / k), Math.round(y / k)];
    }
    updateStage();
  };
  const up = () => { removeEventListener("mousemove", mv); removeEventListener("mouseup", up); if (moved) { commit(); renderRight(); } };
  addEventListener("mousemove", mv); addEventListener("mouseup", up);
}
function startMonitorDrag(e, kind, i, el) {
  e.preventDefault();
  if (isLocked(kind === "topics" ? "tb" : "t")) { toast((kind === "topics" ? "TB" : "T") + " はロック中です", 1200); return; }
  select(kind, i);
  if (kind !== "telops") return;
  const it = D().telops[i], st = $("#stage").getBoundingClientRect(), r = el.getBoundingClientRect();
  const dx = e.clientX - (r.left + r.width / 2), dy = e.clientY - (r.top + r.height / 2);
  let moved = false;
  const mv = ev => {
    if (!moved) { snap(); moved = true; }
    it.pos = [clamp((ev.clientX - dx - st.left) / st.width, 0, 1), clamp((ev.clientY - dy - st.top) / st.height, 0, 1)].map(v => +v.toFixed(4));
    tlKey = ""; updateStage();
  };
  const up = () => { removeEventListener("mousemove", mv); removeEventListener("mouseup", up); if (moved) commit(); };
  addEventListener("mousemove", mv); addEventListener("mouseup", up);
}

/* ---------------- 再生 ---------------- */
const useProc = () => S.has_processed && S.audio && $("#c-proc").checked && !audioStale;
/* 試聴用のカット済み音声(出力の時間軸。サーバーが処理済み音声をカット後の並びにつないで作る)。
   これを基準に鳴らして映像を音に合わせる。カットを飛ばすのが描画待ちで遅れても、削った音が漏れたり音が巻き戻ったりしない。
   カットを変えた直後で作り直しが終わるまでは、従来どおり映像基準で再生する。 */
const pva = new Audio(); pva.preload = "auto";
let pvMode = false;
const pv = { ready: "", want: "", url: "", timer: null };
const pvPieces = () => pieces().map(p => p.gap != null || isMuted("a1") ? { gap: +(p.gap ?? p.e - p.s).toFixed(4) } : { s: +p.s.toFixed(4), e: +p.e.toFixed(4) });  // A1 ミュート = 本編を無音に
// 効果音を出力上の位置に(書き出しの Plan.sfx と同じ: 範囲をカット後に換算し、消えたものは除く)
function pvSfx() {
  const mc = mergedCuts(); if (mc.sfx) return mc.sfx;
  const out = [];
  for (const it of D().sfx || []) { if (sfxMatch(it)) continue; if (isMuted(it.kind === "bgm" ? "bgm" : "se")) continue; const g = isGapIt(it) && gapOf(it); if (isGapIt(it) && !g) continue; const a = g ? g.o + Math.max(0, +it.start) : outTime(it.start), b = g ? g.o + Math.min(+it.end, g.d) : outEnd(it.end); if (b - a > 1e-3) out.push({ file: it.file, at: +a.toFixed(4), len: +(b - a).toFixed(4), in: +(+it.in || 0).toFixed(4), gain_db: +it.gain_db || 0 }); }
  if (fxOk() && !isMuted("se")) out.push(...VFX.se);  // トランジションの効果音(位置はサーバーが書き出しと同じ計算で出す)
  return mc.sfx = out;
}
function pvKey() { const mc = mergedCuts(); return mc.pvKey ??= (S.audio ? S.audio.key : "") + audioVer + S.audio_ver + JSON.stringify(pvPieces()) + JSON.stringify(pvSfx()); }
const pvOn = () => useProc() && !!pv.ready && pv.ready === pvKey();
function pvRequest() {
  clearTimeout(pv.timer);
  if (!S || !S.has_processed || !S.audio || audioStale) return;
  pv.timer = setTimeout(async () => {
    const key = pvKey(); if (key === pv.ready || key === pv.want) return;
    pv.want = key;
    try {
      const r = await api("/api/preview_audio", { pieces: pvPieces(), sfx: pvSfx() });
      if (pv.want !== key) return;
      const wasPv = playing && pvMode; if (wasPv) pause();  // 再生中の試聴用音声の差し替えは、止めてから(差し替えた瞬間に位置が0に戻って飛ぶのを防ぐ)
      pv.url = mediaUrl(r.path, Date.now()); pva.src = pv.url; pv.ready = key;
      if (wasPv || playing && !pvMode && pvOn()) { if (playing) pause(); play(); }  // 再生中に出来上がったら切り替える
    } catch (e) { console.warn("試聴用音声:", e); }
    finally { if (pv.want === key) pv.want = ""; }
  }, 1500);  // 続けて編集している間は作り直さない(1回 300MB 前後を書くので、Q/W の連打のたびに作ると重い)
}
// 出力上の秒 → 元素材の秒(空白の上なら gap:true)
function srcAt(o) {
  const ps = pieces(), p = ps[pieceIdx(o)]; if (!p) return { t: dur(), gap: false };
  return p.gap != null ? { t: p.at, gap: true } : { t: p.s + Math.max(0, o - p.o), gap: false };
}
// これから出るテロップ・差し込み画像を先に読んで展開しておく(切り替わりの瞬間に描画が詰まらないように)。
// 「先」はカット後の時刻で見る(カットをまたいだ先の画像も読む。元素材の時刻で見ると、カットの後ろの画像を読み損ねて初回だけ表示が遅れる)
const preImg = new Map(); let lastPre = 0;
function preloadAhead(now, force) {
  if (!force && now - lastPre < 250) return; lastPre = now;
  const o = curOut();
  for (const it of D().overlays) {
    if (!it.file || !/\.(png|jpe?g|webp|bmp|gif)$/i.test(it.file) || isHidden("v" + trackOf(it))) continue;
    const g = isGapIt(it) && gapOf(it); if (isGapIt(it) && !g) continue;
    const a = g ? g.o + +it.start : outTime(it.start), b = g ? g.o + +it.end : outEnd(it.end); if (a > o + 6 || b <= o) continue;
    const u = pvImgUrl(it.file); if (preImg.has(u)) { const im = preImg.get(u); preImg.delete(u); preImg.set(u, im); continue; }
    const im = new Image(); im.src = u; im.decode().catch(() => {}); preImg.set(u, im);
    if (preImg.size > 120) preImg.delete(preImg.keys().next().value);
  }
}
function setupBgm() {
  if (!S) return;
  const b = D().bgm;
  if (b && bgm.dataset.f !== b.file) { bgm.src = mediaUrl(b.file); bgm.dataset.f = b.file; }
  if (!b) { bgm.removeAttribute("src"); delete bgm.dataset.f; }
  if (b) bgm.volume = isMuted("bgm") ? 0 : clamp(Math.pow(10, (b.volume_db ?? -23.5) / 20) * masterVol(), 0, 1);
}
/* プレビュー音量(確認用のみ。書き出しには影響しない) */
let muted = false;
const masterVol = () => muted ? 0 : Math.pow($("#vol").value / 100, 2);
function applyVol() {
  const v = masterVol();
  vid.volume = v; aud.volume = v; pva.volume = v; setupBgm();
  $("#b-mute").textContent = muted || v === 0 ? "🔇" : "🔊";
  localStorage.setItem("vedit.vol", $("#vol").value);
}
/* 再生中の音量メーター(dBFS)。プレビュー音量(スライダー)は差し引いた「書き出し相当」の値を出す */
let actx = null, anl = null, peakHold = -Infinity, holdAt = 0;
function setupMeter() {
  if (actx) { if (actx.state === "suspended") actx.resume(); return; }
  try {
    actx = new AudioContext(); anl = actx.createAnalyser(); anl.fftSize = 2048; anl.connect(actx.destination);
    for (const el of [vid, vidB, aud, bgm, pva]) actx.createMediaElementSource(el).connect(anl);
  } catch (e) { actx = null; }
}
const toDb = v => v > 1e-6 ? 20 * Math.log10(v) : -Infinity;
const fmtDb = d => isFinite(d) ? d.toFixed(1) : "-∞";
const meterBuf = new Float32Array(2048);
function updateMeter() {
  const el = $("#meter"); if (!el) return;
  if (!actx || !playing) { return; }
  anl.getFloatTimeDomainData(meterBuf);
  let pk = 0, sq = 0; for (const x of meterBuf) { const a = Math.abs(x); if (a > pk) pk = a; sq += x * x; }
  const mv = masterVol(), comp = mv > 0 ? 20 * Math.log10(mv) : 0;
  const pd = mv > 0 ? toDb(pk) - comp : -Infinity, rd = mv > 0 ? toDb(Math.sqrt(sq / meterBuf.length)) - comp : -Infinity;
  const now = performance.now(); if (pd >= peakHold || now - holdAt > 2000) { peakHold = pd; holdAt = now; }
  $("#m-bar").style.width = clamp((pd + 60) / 60 * 100, 0, 100) + "%";
  $("#m-bar").style.background = pd > -1 ? "#ff3b3b" : pd > -6 ? "#f5c542" : "#6ee7b7";
  $("#m-txt").textContent = `Peak ${fmtDb(pd)}  RMS ${fmtDb(rd)}  Hold ${fmtDb(peakHold)} dB`;
}
let pendSeek = null;
for (const v of [vid, vidB]) v.addEventListener("seeked", () => {
  if (v !== vid || pendSeek == null) return;
  const t = pendSeek; pendSeek = null; if (Math.abs(vid.currentTime - t) > 1e-3) vid.currentTime = t;
});
function seek(t, scrub) {
  gpos = null;
  if (gapHold) { endGapHold(); if (playing) { vid.play().catch(() => {}); audWait = useProc(); } }
  t = clamp(t, 0, dur()); tcur = t; sfxStopAll();
  // 止まっている間、前のシーク(デコード)が終わる前に来た要求は最後の1つだけ残し、seeked で出す(スクラブ中に古い位置のデコードを積まない。
  // Olive / mpv の hr-seek の考え。再生ヘッドの表示は待たずに動く)
  if (!playing && vid.seeking) pendSeek = t; else { pendSeek = null; vid.currentTime = t; }
  if (useProc()) aud.currentTime = t;
  if (playing && pvMode) pva.currentTime = outTime(t);
  if (bgm.src && bgm.duration) bgm.currentTime = outTime(t) % bgm.duration;
  updatePlayhead(); updateStage(); updateTracks(); preloadAhead(performance.now(), true);
}
function play() {
  if (!S.info) return;
  if (cutAt(tcur)) seek(cutAt(tcur)[1]);
  if (pendSeek != null) { vid.currentTime = pendSeek; pendSeek = null; }
  const o0 = curOut(), gp = gpos; gpos = null;
  playing = true; $("#b-play").textContent = "⏸"; setupMeter(); peakHold = -Infinity;
  for (const x of pvSfx()) sfxBuffer(x.file);  // 効果音を先にデコードしておく
  vid.muted = useProc();
  pvMode = pvOn();
  if (pvMode) { aud.pause(); pva.playbackRate = rate; pva.currentTime = o0; pva.play().catch(() => {}); }
  else vid.play().catch(() => {});  // 試聴用音声が基準の時は、音が実際に鳴り出してから映像を動かす(tick)
  audWait = useProc() && !pvMode;  // 映像が実際に動き出してから処理済み音声を始める(先に鳴り出して後で巻き戻る「はいどうも はいどうも」を防ぐ)
  if (D().bgm && bgm.duration) { bgm.currentTime = o0 % bgm.duration; bgm.play().catch(() => {}); }
  if (gp && !pvMode) { gapHold = { at: gp.at, d: gp.d, t0: performance.now() - gp.off / rate * 1000 }; vid.pause(); vid.style.visibility = "hidden"; tcur = gp.at - 0.001; }  // 空白の途中から
}
function endGapHold() { if (!gapHold) return; gapHold = null; vid.style.visibility = ""; updateTracks(); }
function pause() { const g = playing ? gapAtOut(curOut()) : null; endGapHold(); if (g) { gpos = g; tcur = g.at; vid.currentTime = g.at; } aud2.pause(); pva.pause(); playing = false; updateSfx(0); audWait = false; $("#b-play").textContent = "▶"; vid.pause(); aud.pause(); bgm.pause(); updateTracks(); updatePlayhead(); updateStage(); }
let lastSync = 0, audWait = false;
/* ダブルバッファ: 再生中は次の区間(カットの後ろ)の頭へ控えの映像を先にシークしておき、継ぎ目で表と入れ替える。
   シークの待ち(キーフレームからのデコード)が再生中に起きないので、カットの多い所でも止まらない */
function nextStart() {
  const ps = pieces(); let i = pieceIdx(curOut());
  for (i++; i < ps.length; i++) if (ps[i].gap == null) return ps[i].s;
  return null;
}
function prepNext() {
  const t = playing ? nextStart() : null; if (t == null || !vidB.src) return;
  if (vidB.dataset.at !== String(t)) { vidB.dataset.at = String(t); vidB.currentTime = t; }
  if (!vidB.paused) vidB.pause();
}
// 継ぎ目で元素材の t へ。控えが t に合わせてあれば入れ替え、無ければ今まで通りシーク
function jumpTo(t) {
  const at = +vidB.dataset.at;
  if (vidB.dataset.at && Math.abs(at - t) < 0.3 && !vidB.seeking && vidB.readyState >= 2) {
    const old = vid; vid = vidB; vidB = old;
    vid.playbackRate = rate; vid.muted = old.muted; vid.style.visibility = old.style.visibility; old.style.visibility = "hidden";
    if (Math.abs(vid.currentTime - t) > 0.05) vid.currentTime = t;
    if (playing) vid.play().catch(() => {});
    old.pause(); old.muted = true; vidB.dataset.at = "";
    return;
  }
  vid.currentTime = t;
}
const vidReady = () => !vid.seeking && vid.readyState >= 3 && !vid.paused;
let lastTick = 0, lastRevSeek = 0;
function tick(now) {
  const dt = Math.min(0.1, (now - (lastTick || now)) / 1000); lastTick = now;
  if (S && shuttle < 0) {  // J: 逆再生(音なし。映像は間引いてシーク)
    let t = tcur + shuttle * dt; const c = mergedCuts().find(c => t > c[0] && t <= c[1]); if (c) t = c[0] - 0.001;
    if (t <= 0) { t = 0; shuttle = 0; }
    tcur = t;
    if (now - lastRevSeek > 90) { lastRevSeek = now; vid.currentTime = t; }
    const sc = $("#scroller"), x = X(tcur);
    if (x < sc.scrollLeft + 40 || x > sc.scrollLeft + sc.clientWidth) sc.scrollLeft = Math.max(0, x - sc.clientWidth + 80);
    updatePlayhead(); updateStage();
  }
  if (S && playing && gapHold) {  // 空白: 黒画面・無音で尺だけ進める
    if (gapHoldPos() >= gapHold.d) { const at = gapHold.at; endGapHold(); jumpTo(at); tcur = at; vid.play().catch(() => {}); audWait = useProc(); }
    prepNext(); updateTracks(); updatePlayhead(); updateStage(); requestAnimationFrame(tick); return;  // 空白に置いたドドン・見出しもここで出す
  }
  if (S && playing && pvMode) {  // 試聴用音声が基準: 音の位置から元素材の時刻を出し、映像をそこへ合わせる
    const o = pva.currentTime, L = totalOut();
    if (pva.ended || o >= L - 0.02) { pause(); seek(dur()); requestAnimationFrame(tick); return; }
    const at = srcAt(o); tcur = at.t;
    if (at.gap) { if (!vid.paused) vid.pause(); }
    else {
      // 音より先に走った映像がカットへ入ったら、音が継ぎ目に来るまで止めて待つ。音の位置へ戻すと、また走ってカットへ入って戻す…を繰り返して詰まる
      const c = cutAt(vid.currentTime);
      if (c && at.t < c[0] && c[0] - at.t < 0.3 && !vid.seeking) { if (!vid.paused) vid.pause(); }
      else {
        if (vid.paused && !pva.paused && !pva.seeking && pva.readyState >= 3) vid.play().catch(() => {});
        if (!vid.seeking && (c || Math.abs(vid.currentTime - at.t) > 0.25)) jumpTo(at.t);
      }
    }
    if (vid.ended) vid.pause();
    updateTracks();
    const sc = $("#scroller"), x = X(tcur);
    if (x > sc.scrollLeft + sc.clientWidth - 40 || x < sc.scrollLeft) sc.scrollLeft = Math.max(0, x - 80);
    updatePlayhead(); updateStage(); updateMeter(); preloadAhead(now); prepNext();
    requestAnimationFrame(tick); return;
  }
  if (S && playing) {
    const tPrev = tcur; preloadAhead(now); prepNext();
    // 処理済み音声が鳴っている間は音を基準にし、映像を音に合わせる。映像基準だと、カットや区切りのシークで映像が一瞬詰まるたびに
    // 音が映像まで巻き戻され、同じ所が二重に聞こえる(試聴用音声の作り直し中に起きていた)
    const lead = useProc() && !audWait && !aud.paused;
    let t = lead ? aud.currentTime : vid.currentTime; const c = cutAt(t);
    if (c) {
      if (c[1] >= dur() - 0.05) { pause(); seek(dur()); }
      else { t = c[1]; jumpTo(t); if (useProc()) aud.currentTime = t; }
    }
    if (lead ? aud.ended : vid.ended) pause();
    const g = gapsN().find(([at]) => at > tPrev + 1e-6 && at <= t + 1e-6 && at < dur() - 0.05);
    if (g && playing) {
      gapHold = { at: g[0], d: g[1], t0: performance.now() }; vid.pause(); aud.pause(); vid.style.visibility = "hidden";
      tcur = g[0] - 0.001; updatePlayhead(); updateStage(); requestAnimationFrame(tick); return;
    }
    if (audWait && vidReady()) { audWait = false; aud.currentTime = vid.currentTime; aud.play().catch(() => {}); }
    vid.muted = useProc();  // 処理済み音声を鳴らす間は映像側の音を必ず消す(二重に聞こえるのを防ぐ)
    updateTracks();
    tcur = t;
    if (lead && !vid.seeking && now - lastSync > 300) { lastSync = now; if (Math.abs(vid.currentTime - t) > 0.25) vid.currentTime = t; }  // 音は巻き戻さず、映像を寄せる
    const sc = $("#scroller"), x = X(tcur);
    if (x > sc.scrollLeft + sc.clientWidth - 40 || x < sc.scrollLeft) sc.scrollLeft = Math.max(0, x - 80);
    updatePlayhead(); updateStage(); updateMeter();
  }
  requestAnimationFrame(tick);
}

/* ---------------- タイムライン ---------------- */
const RULER = 36;  // 再生ヘッドをつまむ帯。上のボタン列と近いと押し間違えるので高めに取る
/* 差し込みのビデオトラック(Premiere の V2, V3…)。overlays の track(既定 2)で載るトラックを決め、番号が大きいほど上に重なる。
   名前は vtracks[n-2](空=番号だけ)。hide = 出力しない映像トラック、mute = 鳴らさない音声トラック(どちらも書き出しにも効く) */
const FIXED = [["tb", "TB 帯", 28, "topics"], ["t", "T 字幕", 34, "telops"], ["v1", "V1 映像", 34, null], ["a1", "A1 音声", 60, null], ["se", "SE 効果音", 34, "sfx"], ["bgm", "BGM", 34, null]];
const AUDIO_LANES = ["a1", "se", "bgm"];
const isAudioLane = id => AUDIO_LANES.includes(id);
const trackOf = it => Math.max(2, Math.round(+it.track || 2));
function vCount() { const p = D(); return Math.max(1, (p.vtracks || []).length, ...p.overlays.map(it => trackOf(it) - 1)); }
const vName = n => { const v = D().vtracks; return v ? v[n - 2] || "" : n === 2 ? "差込" : ""; };
let LANES = [["v2", "V2 差込", 34, "overlays", 2], ...FIXED];
function buildLanes() {
  if (!S) return;
  const vs = []; for (let n = vCount() + 1; n >= 2; n--) vs.push(["v" + n, ("V" + n + " " + vName(n)).trim(), 34, "overlays", n]);
  LANES = [...vs, ...FIXED];
}
const laneOf = (kind, it) => kind === "overlays" ? "v" + trackOf(it) : kind === "sfx" && it && it.kind === "bgm" ? "bgm" : kindLane(kind);
const keyLane = k => { const [kind, j] = k.split(":"); return laneOf(kind, D()[kind][+j]); };
const isHidden = id => (D().hide || []).includes(id);
const isMuted = id => (D().mute || []).includes(id);
let vz = 1, lz = {};  // トラック高さの倍率(縦ズーム)。vz=全トラック、lz[id]=そのトラックだけ
const laneH = (hh, id) => Math.round(hh * vz * (lz[id] || 1));
const laneTop = {}; const calcLanes = () => { buildLanes(); for (const k in laneTop) delete laneTop[k]; let y = RULER; for (const [id, , hh] of LANES) { laneTop[id] = y; y += laneH(hh, id); } laneTop.end = y; }; calcLanes();
const waveH = () => laneH(60, "a1");
const sliderToZoom = v => 0.5 * Math.pow(600, v / 100);
const zoomToSlider = z => Math.round(100 * Math.log(z / 0.5) / Math.log(600));
// 画面xへシーク。詰め表示で空白の上なら空白の中で止める(空白に置いた素材を確認できる)。吸着中は区切りへ
function seekX(cx, shift) {
  const sc = $("#scroller"), o = (cx - sc.getBoundingClientRect().left + sc.scrollLeft) / zoom, g = collapsed ? gapAtOut(o) : null;
  if (g && (seekSnap || shift)) { if (g.off * zoom < 10) g.off = 0; else if ((g.d - g.off) * zoom < 10) return seek(g.at); }  // 吸着中は空白の頭/終わりに寄せる
  if (!g) return seek(scrubT(timeAtX(cx), shift));
  seek(g.at); gpos = g; updatePlayhead(); updateTracks(); updateStage();
}
const timeAtX = cx => { const sc = $("#scroller"); return srcOf((cx - sc.getBoundingClientRect().left + sc.scrollLeft) / zoom); };

/* トラックのロック(Premiere の鍵アイコン)。ロック中のトラックは選択・移動・削除できない。プロジェクトごとに保存 */
const lockKey = () => "vedit.locks:" + (S ? S.dir + "/" + S.name : "");
let locks = new Set();
const isLocked = id => locks.has(id);
const kindLane = kind => kind === "overlays" ? "v2" : (FIXED.find(l => l[3] === kind) || [])[0];
// Shift+クリック: 同じ側(映像=V*/TB/T/V1、音声=A1/SE/BGM)のトラックすべてを、押したトラックの新しい状態にそろえる
const sameSide = id => LANES.map(l => l[0]).filter(x => isAudioLane(x) === isAudioLane(id));
function setLocks(ids, on) {
  for (const id of ids) if (on) locks.add(id); else locks.delete(id);
  localStorage.setItem(lockKey(), JSON.stringify([...locks]));
  for (const k of [...selSet]) if (isLocked(keyLane(k))) selSet.delete(k);
  if (sel && sel.kind !== "cuts" && isLocked(laneOf(sel.kind, D()[sel.kind][sel.i]))) sel = null;
  if (isLocked("v1") || isLocked("a1")) { segSel = null; gapSel = null; trkSel = null; }
  if (isLocked("bgm")) bgmSel = false;
  renderTimeline(); renderRight(); tlKey = ""; updateStage();
}
function toggleLock(id, all) { setLocks(all ? sameSide(id) : [id], !isLocked(id)); }
// 目(映像トラックの出力)・M(音声トラックのミュート)。プロジェクトに保存し、書き出しにも効く。元に戻せる
function setTrackFlag(key, ids, on) {
  mutate(() => { const s = new Set(D()[key] || []); for (const id of ids) if (on) s.add(id); else s.delete(id); if (s.size) D()[key] = [...s]; else delete D()[key]; });
}
const toggleHide = (id, all) => setTrackFlag("hide", all ? sameSide(id) : [id], !isHidden(id));
const toggleMute = (id, all) => setTrackFlag("mute", all ? sameSide(id) : [id], !isMuted(id));
// 番号 n 以上のビデオトラックを d ずらす(差し込みの track、vtracks、hide、ロックをまとめて)
function shiftVTracks(n, d) {
  const p = D(), sh = id => { const m = /^v(\d+)$/.exec(id); return m && +m[1] >= n && +m[1] !== 1 ? "v" + (+m[1] + d) : id; };
  for (const it of p.overlays) if (trackOf(it) >= n) it.track = trackOf(it) + d;
  if (p.hide) p.hide = p.hide.map(sh);
  locks = new Set([...locks].map(sh)); localStorage.setItem(lockKey(), JSON.stringify([...locks]));
}
const vtList = () => { const p = D(), c = vCount(); p.vtracks = Array.from({ length: c }, (_, k) => vName(k + 2)); return p.vtracks; };
function addVTrack(n) {  // n 番として新しいビデオトラックを入れる(今の n 以上は1つ上へ)
  mutate(() => { const v = vtList(); shiftVTracks(n, 1); v.splice(n - 2, 0, ""); vTarget = n; });
}
function deleteVTrack(n) {
  const cnt = D().overlays.filter(it => trackOf(it) === n).length;
  if (vCount() <= 1) { toast("最後のビデオトラックは削除できません", 1500); return; }
  if (cnt && !confirm(`V${n} の差し込み ${cnt} 個も一緒に削除します。よろしいですか?`)) return;
  select(null);
  mutate(() => {
    const p = D(), v = vtList();
    p.overlays = p.overlays.filter(it => trackOf(it) !== n);
    if (p.hide) p.hide = p.hide.filter(x => x !== "v" + n);
    locks.delete("v" + n); shiftVTracks(n + 1, -1); v.splice(n - 2, 1);
    for (const it of p.overlays) if (trackOf(it) === 2) delete it.track;
    vTarget = Math.min(vTarget, vCount() + 1);
  });
}
function deleteEmptyVTracks() {
  const used = new Set(D().overlays.map(trackOf)), empty = []; for (let n = vCount() + 1; n >= 2; n--) if (!used.has(n)) empty.push(n);
  if (empty.length >= vCount()) empty.pop();  // 1本は残す
  if (!empty.length) { toast("空のビデオトラックはありません", 1200); return; }
  mutate(() => { const v = vtList(); for (const n of empty) { if (D().hide) D().hide = D().hide.filter(x => x !== "v" + n); locks.delete("v" + n); shiftVTracks(n + 1, -1); v.splice(n - 2, 1); } vTarget = Math.min(vTarget, vCount() + 1); });
}
function renameVTrack(n) {
  const s = prompt(`V${n} の名前`, vName(n)); if (s == null) return;
  mutate(() => { vtList()[n - 2] = s.trim(); });
}
function moveToTrack(keys, d) {  // 選択中の差し込みを d 本上/下のトラックへ
  const ks = keys.map(k => k.split(":")).filter(([k]) => k === "overlays"); if (!ks.length) return;
  const items = ks.map(([, j]) => D().overlays[+j]), top = vCount() + 1;
  if (items.some(it => trackOf(it) + d < 2 || trackOf(it) + d > top + (d > 0 ? 1 : 0))) { toast("それ以上は動かせません", 1200); return; }
  mutate(() => { if (items.some(it => trackOf(it) + d > top)) vtList().push(""); for (const it of items) { it.track = trackOf(it) + d; if (it.track === 2) delete it.track; } });
}
// ビデオトラックの並べ替え: map[旧番号] = 新番号 で、差し込みの track・名前・hide・ロック・追加先をまとめて付け替える
function remapVTracks(map) {
  const p = D(), names = vtList().slice(), sh = id => { const m = /^v(\d+)$/.exec(id); return m && map[+m[1]] ? "v" + map[+m[1]] : id; };
  for (const it of p.overlays) { it.track = map[trackOf(it)]; if (it.track === 2) delete it.track; }
  names.forEach((s, k) => { p.vtracks[map[k + 2] - 2] = s; });
  if (p.hide) p.hide = p.hide.map(sh);
  locks = new Set([...locks].map(sh)); localStorage.setItem(lockKey(), JSON.stringify([...locks]));
  vTarget = map[vTarget] || vTarget;
}
function moveVTrack(vn, slot) {  // slot = 上から数えた差し込み位置(0=一番上)
  const c = vCount(), ord = []; for (let n = c + 1; n >= 2; n--) ord.push(n);
  const i = ord.indexOf(vn); ord.splice(i, 1); ord.splice(slot > i ? slot - 1 : slot, 0, vn);
  const map = {}; ord.forEach((n, j) => { map[n] = c + 1 - j; });
  if (ord.every(n => map[n] === n)) return;
  mutate(() => remapVTracks(map));
  toast(`V${vn} → V${map[vn]}(上のトラックほど手前に重なります)`, 1500);
}
// トラック名をつまんで上下にドラッグ = ビデオトラックの並べ替え(動かさずに離せば差し込みの追加先にする)
function labelDrag(ev, vn, lb) {
  if (ev.button !== 0 || ev.target.closest("button") || ev.target.classList.contains("lrz")) return;
  ev.preventDefault();
  const y0 = ev.clientY, box = $("#labels"), vls = [...box.querySelectorAll(".lbl[data-vn]")], src = $("#lane-v" + vn);
  let on = false, slot = null, line = null, tline = null, ghost = null;
  const mv = e => {
    if (!on && Math.abs(e.clientY - y0) < 5) return;
    if (!on) {  // つまんだトラック名の写しがカーソルについてくる。落とす位置は トラック名列とタイムラインの両方に太い線で出す
      on = true; lb.classList.add("dragging"); if (src) src.classList.add("dragsrc");
      line = h("div", { class: "lbl-drop" }, h("b", {}, "▶ ここへ")); box.append(line);
      tline = h("div", { class: "lane-drop" }); $("#inner").append(tline);
      const r = lb.getBoundingClientRect(); ghost = h("div", { class: "lbl-ghost" }, "⠿ " + lb.querySelector("span").textContent);
      Object.assign(ghost.style, { left: r.left + "px", width: r.width + "px", height: r.height + "px" }); ghost.dy = y0 - r.top; document.body.append(ghost);
      document.body.style.cursor = "grabbing";
    }
    ghost.style.top = e.clientY - ghost.dy + "px";
    const rs = vls.map(x => x.getBoundingClientRect());
    slot = rs.findIndex(r => e.clientY < r.top + r.height / 2); if (slot < 0) slot = rs.length;
    const y = slot < rs.length ? rs[slot].top : rs[rs.length - 1].bottom;
    line.style.top = y - box.getBoundingClientRect().top + box.scrollTop - 2 + "px";
    const id = slot < vls.length ? vls[slot].dataset.lane : null;
    tline.style.top = (id ? laneTop[id] : laneTop[vls[vls.length - 1].dataset.lane] + laneH(34, vls[vls.length - 1].dataset.lane)) - 2 + "px";
  };
  const up = () => {
    removeEventListener("mousemove", mv); removeEventListener("mouseup", up);
    if (!on) { vTarget = vn; renderTimeline(); return; }
    document.body.style.cursor = ""; line.remove(); tline.remove(); ghost.remove(); lb.classList.remove("dragging"); if (src) src.classList.remove("dragsrc");
    if (slot != null) moveVTrack(vn, slot);
  };
  addEventListener("mousemove", mv); addEventListener("mouseup", up);
}
let vTarget = 2;  // 新しい差し込みを置くトラック(トラック名のクリックで選ぶ = Premiere のターゲット)
function trackMenu(e, id) {
  e.preventDefault();
  const lane = LANES.find(l => l[0] === id), n = lane && lane[4], kind = lane && lane[3], items = [];
  items.push([isLocked(id) ? "トラックのロックを解除" : "トラックをロック", () => toggleLock(id)]);
  if (isAudioLane(id)) items.push([isMuted(id) ? "ミュートを解除" : "トラックをミュート", () => toggleMute(id)]);
  else items.push([isHidden(id) ? "トラックを表示(出力する)" : "トラックを非表示(出力しない)", () => toggleHide(id)]);
  if (kind) items.push(["このトラックのクリップをすべて選択", () => { selSet = new Set(D()[kind].map((it, i) => [kind + ":" + i, it]).filter(([, it]) => laneOf(kind, it) === id).map(([k]) => k)); const l = [...selSet].pop(); sel = l ? { kind, i: +l.split(":")[1] } : null; renderTimeline(); renderRight(); }]);
  items.push(null);
  if (n) {
    items.push(["トラック名を変更…", () => renameVTrack(n)]);
    items.push(["上にビデオトラックを追加", () => addVTrack(n + 1)]);
    items.push(["下にビデオトラックを追加", () => addVTrack(n)]);
    const top = vCount() + 1;  // 上から数えた位置 = top - n
    if (n < top) items.push(["トラックを上へ移動", () => moveVTrack(n, top - n - 1)]);
    if (n > 2) items.push(["トラックを下へ移動", () => moveVTrack(n, top - n + 2)]);
    items.push(["このトラックを削除", () => deleteVTrack(n)]);
    items.push(["空のビデオトラックを削除", deleteEmptyVTracks]);
    if (vTarget !== n) items.push(["ここを差し込みの追加先にする", () => { vTarget = n; renderTimeline(); }]);
  } else items.push(["ビデオトラックを追加(一番上)", () => addVTrack(vCount() + 2)]);
  items.push(null);
  items.push(["すべてのトラックを表示・ミュート解除", () => mutate(() => { delete D().hide; delete D().mute; })]);
  items.push(["すべてのトラックのロックを解除", () => setLocks(LANES.map(l => l[0]), false)]);
  if (lz[id]) items.push(["このトラックの高さを元に戻す", () => { delete lz[id]; renderTimeline(); }]);
  items.push(["トラックの高さを元に戻す(すべて)", () => { vz = 1; lz = {}; renderTimeline(); }]);
  showMenu(e.clientX, e.clientY, items);
}
// 見える所(と左右の余白)のクリップだけ作る。全部作ると千個単位の要素の配置計算で、編集・選択・ズームのたびに 100ms 近く止まる。
// 余白の外までスクロールしたら作り直す(scroll の中で同期して作るので、空の所は描画されない)
let tlWin = [-Infinity, Infinity];
function tlRange() { const sc = $("#scroller"), W = sc.clientWidth || 1600; return [sc.scrollLeft - W * 1.5, sc.scrollLeft + W * 2.5]; }
function tlCheckWin() { const sc = $("#scroller"); if (S && (sc.scrollLeft < tlWin[0] || sc.scrollLeft + sc.clientWidth > tlWin[1])) renderTimeline(); }
function renderTimeline() {
  if (!S) return;
  const inner = $("#inner"), labels = $("#labels"), p = D();
  const keep = [...inner.querySelectorAll(":scope > .marquee, :scope > .lane-drop")];  // ドラッグ中の範囲選択の枠などは残す
  calcLanes(); inner.replaceChildren(); labels.replaceChildren();
  if (vTarget > vCount() + 1) vTarget = 2;
  inner.style.width = tlen() * zoom + 400 + "px"; inner.style.height = laneTop.end + "px";
  const [vL, vR] = tlWin = tlRange(), vis = (x0, x1) => x1 >= vL && x0 <= vR;
  labels.append(h("div", { class: "rulerlbl", style: { height: RULER + "px" } },
    h("button", { id: "b-seeksnap", class: "seeksnap" + (seekSnap ? " on" : ""), title: "シーク吸着: オンにすると再生ヘッドのドラッグが区切り線(分割点・カットの継ぎ目・各トラックの白い線)で止まる。オフでも Shift を押しながらドラッグすれば止まる", onclick: () => setSeekSnap(!seekSnap) }, "🧲 区切りで止める")));
  LANES.forEach(([id, name, hh, , vn], n) => {
    const tip = "(Shift+クリックで" + (isAudioLane(id) ? "音声" : "映像") + "トラックすべて)";
    const lk = h("button", { class: "lock" + (isLocked(id) ? " on" : ""), title: "トラックをロック / 解除" + tip, onclick: ev => { ev.stopPropagation(); toggleLock(id, ev.shiftKey); } }, isLocked(id) ? "🔒" : "🔓");
    const off = isAudioLane(id) ? isMuted(id) : isHidden(id);
    const tg = isAudioLane(id)
      ? h("button", { class: "tgl mute" + (off ? " on" : ""), title: "ミュート / 解除" + tip, onclick: ev => { ev.stopPropagation(); toggleMute(id, ev.shiftKey); } }, "M")
      : h("button", { class: "tgl eye" + (off ? " on" : ""), title: "表示 / 非表示(出力の切り替え)" + tip, onclick: ev => { ev.stopPropagation(); toggleHide(id, ev.shiftKey); } }, off ? "◌" : "👁");
    const lb = h("div", { class: "lbl" + (isLocked(id) ? " locked" : "") + (off ? " off" : "") + (vn && vn === vTarget ? " target" : ""), title: vn ? "クリック=差し込みの追加先 / 上下にドラッグ=トラックの並べ替え / 右クリック=トラックのメニュー" : "右クリック=トラックのメニュー", "data-vn": vn || null, "data-lane": id, style: { height: laneH(hh, id) + "px" } }, vn ? h("i", { class: "grip", title: "つまんで上下にドラッグ=トラックの並べ替え" }, "⠿") : null, lk, tg, h("span", {}, name), h("i", { class: "lrz", title: "上下にドラッグ=このトラックだけ高さを変える(ダブルクリックで元に戻す)", onmousedown: ev => laneResize(ev, id, hh), ondblclick: ev => { ev.stopPropagation(); delete lz[id]; renderTimeline(); } }));
    lb.addEventListener("contextmenu", ev => trackMenu(ev, id));
    if (vn) lb.addEventListener("mousedown", ev => labelDrag(ev, vn, lb));
    labels.append(lb);
    inner.append(h("div", { class: "lane" + (n % 2 ? " alt" : "") + (isLocked(id) ? " locked" : "") + (off ? " off" : ""), id: "lane-" + id, style: { top: laneTop[id] + "px", height: laneH(hh, id) + "px" } }));
  });
  const rc = h("canvas", { class: "sticky", id: "rulerc", height: RULER }); const rl = h("div", { class: "lane", id: "rulerlane", style: { top: 0, height: RULER + "px" } }, rc);
  inner.append(rl);
  const ac = h("canvas", { class: "sticky", id: "wavec", height: waveH() }); $("#lane-a1").append(ac);
  const srcName = D().source.split(/[\\/]/).pop();
  // 要素はトラックごとにまとめて足す(1個ずつ画面の木に足すより速い)
  const frag = new Map(), add = (l, el) => { let f = frag.get(l); if (!f) frag.set(l, f = document.createDocumentFragment()); f.append(el); };
  for (const [a, b] of segments()) if (vis(X(a), X(b))) for (const l of ["v1", "a1"]) add(l, segEl(a, b, l === "v1" ? srcName : ""));
  for (const [at, d] of gapsN()) if (vis(X(at) - d * zoom - 3, X(at) + 3)) for (const l of ["v1", "a1"]) add(l, gapEl(at, d, l === "v1"));
  (p.vmute || []).forEach((r, i) => add("v1", trkEl("vmute", i, r[0], r[1], "黒")));
  (p.amute || []).forEach((r, i) => add("a1", trkEl("amute", i, r[0], r[1], "無音")));
  (p.aslip || []).forEach((sl, i) => { add("a1", trkEl(null, i, sl.a, sl.b, "無音(移動元)")); add("a1", slipEl(sl, i)); });
  for (const kind of LIST) p[kind].forEach((it, i) => { const [x0, x1] = itX(it); if (vis(x0, x1)) add(laneOf(kind, it), clipEl(kind, it, i)); });
  p.cuts.forEach((c, i) => { if (vis(X(Math.min(c[0], c[1])), X(Math.max(c[0], c[1])) + 2)) for (const l of ["v1", "a1"]) add(l, cutEl(c, i)); });
  // 分割線: V1/A1 は分割点、テロップ等は「前のクリップの終わり = 次のクリップの頭」の所に線
  for (const t of p.splits) if (vis(X(t), X(t))) for (const l of ["v1", "a1"]) add(l, h("div", { class: "split", style: { left: X(t) + "px" } }));
  for (const [l, ts] of clipSplits()) for (const t of ts) if (vis(X(t), X(t))) add(l, h("div", { class: "split", style: { left: X(t) + "px" } }));
  for (const [l, f] of frag) $("#lane-" + l).append(f);
  if (p.bgm) $("#lane-bgm").append(bgmEl(p.bgm));
  if (markIn != null) inner.append(h("div", { class: "mark", id: "mark" }));
  inner.append(h("div", { id: "playhead" }), ...keep);
  updateMark(); updatePlayhead(); fitCanvas(); drawRuler(); drawWave(); updateVScroll(); stickRuler();
}
// BGM: クリックで選択(左の「素材」でBGMの設定)、Delete で外す
function bgmEl(b) {
  const el = h("div", { class: "clip c-bgm" + (bgmSel ? " sel" : ""), title: "BGM  クリック=選択(左パネルで音量など) / Delete=外す", style: { left: 0, width: totalOut() * zoom + "px", cursor: "pointer" } }, "♪ " + b.file.split(/[\\/]/).pop() + `  ${b.volume_db ?? -23.5}dB`);
  const pick = e => { if (e.ctrlKey || e.shiftKey) bgmSel = !bgmSel; else { select(null); segSel = null; gapSel = null; bgmSel = true; } leftTab = "media"; renderLeft(); renderTimeline(); };
  el.addEventListener("mousedown", e => { if (e.button !== 0) return; e.stopPropagation(); if (isLocked("bgm")) return; pick(e); });
  el.addEventListener("contextmenu", e => { e.preventDefault(); if (isLocked("bgm")) return; if (!bgmSel) pick(e); clipMenu(e); });
  return el;
}
function clipEl(kind, it, i) {
  const label = kind === "overlays" ? (it.file ? it.file.split(/[\\/]/).pop() : "✦ " + (it.kind || "?")) : kind === "sfx" ? `♪ ${it.file.split(/[\\/]/).pop()} ${+it.gain_db || 0}dB` : it.text.replace(/\n/g, " ");
  const el = h("div", { class: `clip c-${kind}` + (kind === "sfx" && it.kind === "bgm" ? " c-bgm" : "") + (selSet.has(kind + ":" + i) ? " sel" : ""), title: label }, label);
  const [x0, x1] = itX(it); el.style.left = x0 + "px"; el.style.width = Math.max(4, x1 - x0) + "px";
  if (isGapIt(it)) { el.classList.add("ingap"); el.title = "空白の中: " + label; }
  if (kind === "sfx") el.prepend(clipWave(it, x0, Math.max(4, x1 - x0)));
  el.addEventListener("dblclick", () => { const ta = $("#right-body textarea"); if (ta) { ta.focus(); ta.select(); } });
  el.addEventListener("contextmenu", e => { e.preventDefault(); if (!selSet.has(kind + ":" + i)) select(kind, i, true); clipMenu(e); });
  el.dataset.k = kind + ":" + i;
  el.addEventListener("mousemove", e => { el.style.cursor = edgeMode(el, e.clientX) === "m" ? "" : "ew-resize"; });
  el.addEventListener("mousedown", e => clipDown(e, kind, i, el, edgeMode(el, e.clientX)));
  return el;
}
// 効果音・BGM の波形(素材ファイルの波形を、そのクリップで鳴る範囲だけ)。書き出しと同じく、出力上の経過時間で素材を進める
const fwave = {};
function clipWave(it, x0, w) {
  const H = 40, c = h("canvas", { class: "cwave", height: H }), W = Math.min(Math.round(w), 8000); c.width = Math.max(1, W);
  const wv = fwave[it.file];
  if (wv === undefined) { fwave[it.file] = null; api("/api/waveform?p=" + encodeURIComponent(it.file)).then(d => { fwave[it.file] = d && d.file && d.peaks ? d : false;  /* 古いサーバーは本編の波形を返すので使わない */ renderTimeline(); }).catch(() => { fwave[it.file] = false; }); }
  if (!wv) return c;
  const g = c.getContext("2d"), r = wv.rate, pk = wv.peaks, k = w / W, gp = isGapIt(it), o0 = gp ? 0 : outTime(+it.start), fin = +it.in || 0;
  g.fillStyle = "rgba(255,255,255,.55)";
  const at = px => gp ? fin + px / zoom : fin + outTime(srcOf((x0 + px) / zoom)) - o0;
  for (let x = 0; x < W; x++) {
    const a = Math.floor(at(x * k) * r), b = Math.max(a + 1, Math.floor(at((x + 1) * k) * r));
    let m = 0; for (let i = Math.max(0, a); i < b && i < pk.length; i++) if (pk[i] > m) m = pk[i];
    const hh = (m > 0 ? Math.max(0, 1 + Math.log10(m / 255) * 20 / 48) : 0) * (H - 2); if (hh > 0) g.fillRect(x, (H - hh) / 2, 1, hh);
  }
  return c;
}
function clipDown(e, kind, i, el, mode) {
  if (e.button !== 0) return; e.stopPropagation();
  if (e.ctrlKey || e.shiftKey) { toggleSel(kind, i); return; }  // Premiere: Ctrl/Shift+クリックで追加選択
  if (!selSet.has(kind + ":" + i)) select(kind, i, true); else { sel = { kind, i }; renderRight(); }
  startDrag(e, kind, i, mode, el);
}
// 端のつまみは位置で決める(見た目の i ではなく)。端から8pxまで。短いクリップは幅の1/3まで(真ん中は移動に残す)
const EDGE = 8;
function edgeMode(el, x) { const r = el.getBoundingClientRect(), E = Math.max(3, Math.min(EDGE, r.width / 3)); return x - r.left < E ? "l" : r.right - x < E ? "r" : "m"; }
// クリップの外側(トラックの空き)でも、端から6px以内ならその端をつまめる
function edgeNear(e) {
  const lane = e.target.classList && e.target.classList.contains("lane") ? e.target : null; if (!lane) return null;
  let best = null, bd = 6;
  for (const c of lane.querySelectorAll(":scope > .clip[data-k]")) {
    const r = c.getBoundingClientRect();
    if (Math.abs(e.clientX - r.right) < bd) { bd = Math.abs(e.clientX - r.right); best = [c, "r"]; }
    if (Math.abs(e.clientX - r.left) < bd) { bd = Math.abs(e.clientX - r.left); best = [c, "l"]; }
  }
  return best;
}
$("#scroller").addEventListener("mousedown", e => {
  if (e.button !== 0 || !S || cutTool || tool === "razor") return;
  const hit = edgeNear(e); if (!hit) return;
  const [kind, j] = hit[0].dataset.k.split(":"); e.preventDefault(); clipDown(e, kind, +j, hit[0], hit[1]);
}, true);
$("#scroller").addEventListener("mousemove", e => { if (e.target.classList && e.target.classList.contains("lane")) e.target.style.cursor = edgeNear(e) ? "ew-resize" : ""; });
function cutEl(c, i) {
  const a = Math.min(c[0], c[1]), b = Math.max(c[0], c[1]);
  const el = h("div", { class: "cutr" + (sel && sel.kind === "cuts" && sel.i === i ? " sel" : ""), title: `カット ${fmtT(a)}–${fmtT(b)}`, "data-ci": i }, ...(collapsed ? [] : [h("i", { class: "l" }), h("i", { class: "r" })]));
  el.style.left = X(a) + "px"; el.style.width = Math.max(2, X(b) - X(a)) + "px";
  if (collapsed) { el.classList.add("seam"); el.style.width = "2px"; }  // 詰め表示では継ぎ目の赤線だけ(クリップの端をドラッグでリップルトリム)
  el.addEventListener("mousedown", e => { e.stopPropagation(); select("cuts", i, true); if (collapsed) return; startDrag(e, "cuts", i, e.target.classList.contains("l") ? "l" : e.target.classList.contains("r") ? "r" : "m", el); });
  return el;
}
function startDrag(e, kind, i, mode, el) {
  e.preventDefault();
  if (kind !== "cuts" && isGapIt(D()[kind][i])) return gapItemDrag(e, kind, i, mode, el);
  const it = D()[kind][i], [s0, e0] = getR(kind, it), x0 = e.clientX, y0 = e.clientY, in0 = +it.in || 0; let moved = false;
  const snapPts = [tcur, 0, dur()];
  for (const k of [...LIST, "cuts"]) D()[k].forEach((o, j) => { if (!(k === kind && j === i) && !isGapIt(o)) snapPts.push(...getR(k, o)); });
  const group = kind !== "cuts" && mode === "m" ? [...selSet].map(k => k.split(":")).filter(([k, j]) => !(k === kind && +j === i) && !isGapIt(D()[k][+j])).map(([k, j]) => [D()[k][+j], getR(k, D()[k][+j]), k]) : [];
  const sn = t => { if (!snapOn) return t; let best = t, bd = 8 / zoom; for (const q of snapPts) if (Math.abs(q - t) < bd) { bd = Math.abs(q - t); best = q; } return best; };
  const mv = ev => {
    const dx = (ev.clientX - x0) / zoom, mapd = (b0, d) => collapsed ? srcOf(outTime(b0) + d) : b0 + d;
    if (!moved && Math.abs(ev.clientX - x0) < 3 && !(kind === "overlays" && mode === "m" && Math.abs(ev.clientY - y0) >= 8)) return;
    if (!moved) { snap(); moved = true; }
    let a = s0, b = e0;
    if (mode === "m" && Math.abs(ev.clientX - x0) < 3) { /* 上下だけ動かした = 時刻はそのまま(トラックの移動だけ) */ }
    else if (mode === "m") { const len = e0 - s0; a = clamp(sn(mapd(s0, dx)), 0, dur() - len); if (Math.abs(a - mapd(s0, dx)) < 1e-9) a = clamp(sn(mapd(e0, dx)) - len, 0, dur() - len); b = a + len; }
    else if (mode === "l") { a = clamp(sn(mapd(s0, dx)), 0, e0 - 0.05); }
    else { b = clamp(sn(mapd(e0, dx)), s0 + 0.05, dur()); }
    setR(kind, it, +a.toFixed(3), +b.toFixed(3));
    if (kind === "sfx" && mode === "l") it.in = +Math.max(0, in0 + inDelta(it, s0, a)).toFixed(3);  // 効果音の左端 = 音の頭を削る(鳴る位置は動かない)
    for (const [o, [oa, ob], k] of group) { const d = clamp(a - s0, -oa, dur() - ob); setR(k, o, +(oa + d).toFixed(3), +(ob + d).toFixed(3)); }
    let retrack = false;
    if (kind === "overlays" && mode === "m") {  // 上下にドラッグ = 別のビデオトラックへ(選択中の差し込みも同じ本数だけ)
      const y = ev.clientY - $("#inner").getBoundingClientRect().top, ln = LANES.find(l => l[4] && y >= laneTop[l[0]] && y < laneTop[l[0]] + laneH(l[2], l[0]));
      const d = ln ? ln[4] - trackOf(it) : 0, ovs = [it, ...group.filter(g => g[2] === "overlays").map(g => g[0])], top = vCount() + 1;
      if (d && !isLocked(ln[0]) && ovs.every(o => trackOf(o) + d >= 2 && trackOf(o) + d <= top)) { for (const o of ovs) { o.track = trackOf(o) + d; if (o.track === 2) delete o.track; } retrack = true; }
    }
    if (group.length || retrack) { renderTimeline(); return; }
    for (const x of document.querySelectorAll(kind === "cuts" ? ".cutr.sel" : ".clip.sel")) { x.style.left = X(a) + "px"; x.style.width = Math.max(2, X(b) - X(a)) + "px"; }
    if (kind === "sfx") { const [l, r] = itX(it), c = el.querySelector(".cwave"); if (c) c.replaceWith(clipWave(it, l, Math.max(4, r - l))); }  // 波形は縮めずに描き直す(端を切ったら切った所までの波形)
    if (kind === "cuts") cutsCache = null;
    if (mode !== "m") { tcur = mode === "l" ? a : b; }
    updatePlayhead(); updateStage();
  };
  const up = () => { removeEventListener("mousemove", mv); removeEventListener("mouseup", up); if (moved) commit(); };
  addEventListener("mousemove", mv); addEventListener("mouseup", up);
}
function updatePlayhead() { const p = $("#playhead"); if (p) p.style.left = (collapsed ? curOut() * zoom : X(tcur)) + "px"; }
// 空白の中の素材のドラッグ: 空白の中だけで動かす・端を詰める(詰め表示のみ。元の並びの表示では空白に幅が無い)
function gapItemDrag(e, kind, i, mode, el) {
  const it = D()[kind][i], g = gapOf(it); if (!g || !collapsed || isLocked(laneOf(kind, it))) return;
  const s0 = +it.start, e0 = +it.end, len = e0 - s0, x0 = e.clientX, in0 = +it.in || 0; let moved = false;
  const mv = ev => {
    const dx = (ev.clientX - x0) / zoom; if (!moved && Math.abs(ev.clientX - x0) < 3) return; if (!moved) { snap(); moved = true; }
    let a = s0, b = e0;
    if (mode === "m") { a = clamp(s0 + dx, 0, Math.max(0, g.d - len)); b = a + len; }
    else if (mode === "l") a = clamp(s0 + dx, 0, e0 - 0.05);
    else b = clamp(e0 + dx, s0 + 0.05, g.d);
    it.start = +a.toFixed(3); it.end = +b.toFixed(3);
    if (kind === "sfx" && mode === "l") it.in = +Math.max(0, in0 + a - s0).toFixed(3);
    const [l, r] = itX(it); el.style.left = l + "px"; el.style.width = Math.max(4, r - l) + "px";
    if (kind === "sfx") { const c = el.querySelector(".cwave"); if (c) c.replaceWith(clipWave(it, l, Math.max(4, r - l))); }
    updateStage();
  };
  const up = () => { removeEventListener("mousemove", mv); removeEventListener("mouseup", up); if (moved) commit(); };
  addEventListener("mousemove", mv); addEventListener("mouseup", up);
}
const gapHoldPos = () => Math.min(gapHold.d, (performance.now() - gapHold.t0) / 1000 * rate);
function updateMark() {
  const m = $("#mark"); if (!m || markIn == null) return;
  const a = markIn, b = markOut ?? markIn; m.style.left = X(Math.min(a, b)) + "px"; m.style.width = Math.max(2, X(Math.max(a, b)) - X(Math.min(a, b))) + "px";
}
function fitCanvas() {
  const w = $("#scroller").clientWidth;
  for (const c of [$("#rulerc"), $("#wavec")]) if (c && c.width !== w) c.width = w;
}
function drawRuler() {
  const c = $("#rulerc"); if (!c) return; const g = c.getContext("2d"), sc = $("#scroller"), w = c.width;
  g.clearRect(0, 0, w, RULER); g.fillStyle = "#2d2d34"; g.fillRect(0, 0, w, RULER);
  const t0 = sc.scrollLeft / zoom, t1 = (sc.scrollLeft + w) / zoom;
  const step = [0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1200].find(s => s * zoom >= 80) || 1800;
  g.fillStyle = "#8d8d99"; g.strokeStyle = "#555"; g.font = "11px sans-serif";
  for (let t = Math.floor(t0 / step) * step; t < t1; t += step) {
    const x = Math.round(t * zoom - sc.scrollLeft) + 0.5; g.beginPath(); g.moveTo(x, 20); g.lineTo(x, RULER); g.stroke(); g.fillText(fmtT(t, step < 1), x + 3, 15);
  }
}
function drawWave() {
  const c = $("#wavec"); if (!c) return; const g = c.getContext("2d"), sc = $("#scroller"), w = c.width, H = c.height;
  g.clearRect(0, 0, w, H);
  if (!wave) return;
  g.fillStyle = useProcColor(); const r = wave.rate, pk = wave.peaks;
  for (let x = 0; x < w; x++) {
    if (collapsed && inGap((sc.scrollLeft + x) / zoom)) continue;
    const a = Math.floor(srcOf((sc.scrollLeft + x) / zoom) * r), b = Math.min(a + Math.ceil(r / zoom) + 1, Math.max(a + 1, Math.floor(srcOf((sc.scrollLeft + x + 1) / zoom) * r)));
    let m = 0; for (let i = a; i < b && i < pk.length; i++) if (pk[i] > m) m = pk[i];
    // 高さは dB(-48〜0dB)で。リニアだと小さめの声がほぼ線になって無音に見えるため
    const hh = Math.max(1, (m > 0 ? Math.max(0, 1 + Math.log10(m / 255) * 20 / 48) : 0) * (H - 4)); g.fillRect(x, (H - hh) / 2, 1, hh);
  }
}
const useProcColor = () => S.has_processed && !audioStale ? "#6ee7b7" : "#7c8aa5";
$("#scroller").addEventListener("scroll", () => { tlCheckWin(); drawRuler(); drawWave(); $("#labels").scrollTop = $("#scroller").scrollTop; updateVScroll(); stickRuler(); });
// 上下に送ってもルーラー(シークバー)と再生ヘッドのつまみは上に残す
function stickRuler() {
  const st = $("#scroller").scrollTop + "px", rl = $("#rulerlane"), lb = $("#labels .rulerlbl"), ph = $("#playhead");
  if (rl) rl.style.top = st; if (lb) lb.style.top = st; if (ph) ph.style.setProperty("--st", st);
}
// 右端(メーターの左)の縦スクロールバー: つまみをドラッグでトラックを上下に送る。空いた所のクリックで1画面ぶん、ホイールでも送る
function updateVScroll() {
  const sc = $("#scroller"), vs = $("#vscroll"), th = $("#vthumb"); if (!vs) return;
  const H = vs.clientHeight, ch = sc.clientHeight, sh = sc.scrollHeight, hh = Math.max(24, H * ch / Math.max(1, sh));
  vs.classList.toggle("off", sh <= ch + 1);
  th.style.height = hh + "px"; th.style.top = (sh > ch ? (H - hh) * sc.scrollTop / (sh - ch) : 0) + "px";
}
$("#vthumb").addEventListener("mousedown", e => {
  if (e.button !== 0) return; e.preventDefault(); e.stopPropagation();
  const sc = $("#scroller"), th = e.currentTarget, y0 = e.clientY, s0 = sc.scrollTop;
  const k = (sc.scrollHeight - sc.clientHeight) / Math.max(1, $("#vscroll").clientHeight - th.offsetHeight);
  th.classList.add("drag");
  const mv = ev => { sc.scrollTop = s0 + (ev.clientY - y0) * k; };
  const up = () => { removeEventListener("mousemove", mv); removeEventListener("mouseup", up); th.classList.remove("drag"); };
  addEventListener("mousemove", mv); addEventListener("mouseup", up);
});
$("#vscroll").addEventListener("mousedown", e => {
  if (e.button !== 0 || e.target !== e.currentTarget) return;
  const sc = $("#scroller"), r = $("#vthumb").getBoundingClientRect(); sc.scrollTop += (e.clientY < r.top ? -1 : 1) * sc.clientHeight * 0.9;
});
$("#vscroll").addEventListener("wheel", e => { e.preventDefault(); $("#scroller").scrollTop += e.deltaY || e.deltaX; }, { passive: false });
// トラック名の上でホイール = 縦ズーム(全トラックの高さの拡大縮小)。Alt / Ctrl + ホイール = カーソルの下のトラックだけ
$("#labels").addEventListener("wheel", e => {
  e.preventDefault(); const f = (e.deltaY || e.deltaX) < 0 ? 1.15 : 1 / 1.15;
  const lb = (e.altKey || e.ctrlKey) && e.target.closest(".lbl[data-lane]");
  if (lb) { const id = lb.dataset.lane; lz[id] = clamp((lz[id] || 1) * f, 0.5, 5); if (Math.abs(lz[id] - 1) < 0.05) delete lz[id]; }
  else vz = clamp(vz * f, 0.6, 3);
  renderTimeline();
}, { passive: false });
// トラック名の下端をドラッグ = そのトラックだけ高さを変える
function laneResize(e, id, hh) {
  if (e.button !== 0) return; e.preventDefault(); e.stopPropagation();
  const y0 = e.clientY, h0 = laneH(hh, id);
  const mv = ev => { lz[id] = clamp((h0 + ev.clientY - y0) / (hh * vz), 0.5, 5); renderTimeline(); };
  const up = () => { removeEventListener("mousemove", mv); removeEventListener("mouseup", up); };
  addEventListener("mousemove", mv); addEventListener("mouseup", up);
}
$("#scroller").addEventListener("wheel", e => {
  // Premiere 準拠: ホイール=横スクロール / Ctrl・Shift+ホイール=縦スクロール / Alt+ホイール=ズーム(カーソル位置基準)
  if (e.altKey) { e.preventDefault(); setZoom(clamp(zoom * ((e.deltaY || e.deltaX) < 0 ? 1.2 : 1 / 1.2), 0.5, 300), e.clientX); }
  else if (e.shiftKey || e.ctrlKey) { $("#scroller").scrollTop += e.deltaY || e.deltaX; e.preventDefault(); }
  else if (Math.abs(e.deltaY) > Math.abs(e.deltaX)) { $("#scroller").scrollLeft += e.deltaY; e.preventDefault(); }
}, { passive: false });
$("#zoom").addEventListener("input", e => setZoom(sliderToZoom(+e.target.value)));
function setZoom(z, cx) {
  const sc = $("#scroller"), anchorX = cx != null ? cx - sc.getBoundingClientRect().left : sc.clientWidth / 2, t = (sc.scrollLeft + anchorX) / zoom;
  zoom = z; $("#zoom").value = zoomToSlider(z); renderTimeline(); sc.scrollLeft = t * zoom - anchorX; drawRuler(); drawWave();
}
// 空きをクリック/ドラッグでシーク。カットツール時は V1 をドラッグして範囲カット
// カットツール中はクリップの上でもドラッグで範囲カット(クリップの移動より先に拾う)
$("#scroller").addEventListener("mousedown", e => {
  if (e.button !== 0 || !S || !cutTool || e.target === e.currentTarget) return;
  const y = e.clientY - $("#scroller").getBoundingClientRect().top;
  if (y >= laneTop.v1 && y < laneTop.a1) { e.stopPropagation(); e.preventDefault(); tlDown(e); }
}, true);
$("#scroller").addEventListener("mousedown", e => tlDown(e));
function tlDown(e) {
  if (e.button !== 0 || !S) return;
  const y = e.clientY - $("#scroller").getBoundingClientRect().top;
  if (cutTool && y >= laneTop.v1 && y < laneTop.a1) {
    const t0 = timeAtX(e.clientX); let t1 = t0;
    const ghost = h("div", { class: "cutr", style: { left: X(t0) + "px", width: "2px" } }); $("#lane-v1").append(ghost);
    const mv = ev => { t1 = clamp(timeAtX(ev.clientX), 0, dur()); ghost.style.left = X(Math.min(t0, t1)) + "px"; ghost.style.width = Math.abs(X(t1) - X(t0)) + "px"; };
    const up = () => { removeEventListener("mousemove", mv); removeEventListener("mouseup", up); ghost.remove(); if (Math.abs(t1 - t0) > 0.05) mutate(() => D().cuts.push([+Math.min(t0, t1).toFixed(3), +Math.max(t0, t1).toFixed(3)])); };
    addEventListener("mousemove", mv); addEventListener("mouseup", up); return;
  }
  if (tool === "razor") { splitAt(snapT(timeAtX(e.clientX))); return; }
  if (y >= RULER && Math.abs(e.clientX - $("#playhead").getBoundingClientRect().left) > 6) { marquee(e); return; }  // Premiere: トラックの空きはクリックで選択解除/ドラッグで範囲選択。再生ヘッドはルーラーか赤い線(±6px)をつまんで動かす
  if (playing) pause();  // シークしたら止めたままにする(離しても再生し直さない)
  let shift = e.shiftKey;
  seekX(e.clientX, shift);
  // 再生ヘッドをつまんで端へ寄せたら、その方向へタイムラインを送る(端からの距離が遠いほど速く)
  const sc = $("#scroller"); let cx = e.clientX, raf = 0;
  const edge = () => {
    const r = sc.getBoundingClientRect(), M = 40, d = cx < r.left + M ? cx - (r.left + M) : cx > r.right - M ? cx - (r.right - M) : 0;
    if (d) { const before = sc.scrollLeft; sc.scrollLeft += clamp(d * 0.5, -60, 60); if (sc.scrollLeft !== before) seekX(clamp(cx, r.left, r.right), shift); }
    raf = requestAnimationFrame(edge);
  };
  raf = requestAnimationFrame(edge);
  const mv = ev => { cx = ev.clientX; shift = ev.shiftKey; const r = sc.getBoundingClientRect(); seekX(clamp(cx, r.left, r.right), shift); };
  const up = () => { cancelAnimationFrame(raf); removeEventListener("mousemove", mv); removeEventListener("mouseup", up); };
  addEventListener("mousemove", mv); addEventListener("mouseup", up);
}

/* ---------------- 選択・追加・削除 ---------------- */
// 選択だけ変わった時は、タイムラインを作り直さずに印(.sel)だけ付け替える(クリックしてからドラッグが始まるまでの待ちを無くす)
function selMarks() {
  for (const el of document.querySelectorAll("#inner .clip[data-k]")) el.classList.toggle("sel", selSet.has(el.dataset.k));
  for (const el of document.querySelectorAll("#inner .cutr[data-ci]")) el.classList.toggle("sel", !!sel && sel.kind === "cuts" && sel.i === +el.dataset.ci);
}
function select(kind, i, keepInspector) {
  const light = !bgmSel && !trkSel && !(kind && (segSel || gapSel));  // 映像/音声の区間・空白・BGM などの選択が外れる時は作り直す
  sel = kind ? { kind, i } : null; selSet = new Set(kind ? [kind + ":" + i] : []); if (kind) { segSel = null; gapSel = null; } bgmSel = false; trkSel = null;
  if (kind && kind !== "cuts" && rightTab !== "clip" && rightTab !== "style" && rightTab !== "history") rightTab = "clip";
  if (kind && kind !== "cuts" && keepInspector !== "style" && rightTab !== "history") rightTab = "clip";  // ヒストリーを見ている時は切り替えない(Premiere と同じ)
  if (light) selMarks(); else renderTimeline();
  renderRight(); tlKey = ""; updateStage();
}
function addItem(kind, item) {
  mutate(() => { D()[kind].push(item); D()[kind].sort((a, b) => a.start - b.start); });
  select(kind, D()[kind].indexOf(item));
}
const addTelop = () => addItem("telops", { start: +tcur.toFixed(2), end: +Math.min(dur(), tcur + 3).toFixed(2), text: "テロップ" });
const addTopic = () => addItem("topics", { start: +tcur.toFixed(2), end: +Math.min(dur(), tcur + 10).toFixed(2), text: "トピック" });
async function addOverlay() {
  const r = await api("/api/pick", { kind: "media" }); if (!r.path) return;
  addItem("overlays", { file: relPath(r.path), start: +tcur.toFixed(2), end: +Math.min(dur(), tcur + 3).toFixed(2), pos: "full", ...(vTarget > 2 ? { track: vTarget } : {}) });
}
async function addSfx() {
  const r = await api("/api/pick", { kind: "audio" }); if (!r.path) return;
  const f = relPath(r.path); if (r.duration) (S.sfx_dur ||= {})[f] = r.duration;
  const last = D().sfx[D().sfx.length - 1];
  addItem("sfx", { file: f, start: +tcur.toFixed(3), end: +(tcur + (r.duration || 1)).toFixed(3), gain_db: last ? +last.gain_db || 0 : 0 });
}
function relPath(p) {
  const n = s => s.replace(/\\/g, "/").toLowerCase(), d = n(S.dir) + "/";
  return n(p).startsWith(d) ? p.replace(/\\/g, "/").slice(d.length) : p;
}
function deleteSel() {
  const keys = selSet.size ? [...selSet] : sel ? [sel.kind + ":" + sel.i] : []; if (!keys.length) return;
  sel = null; selSet = new Set();
  mutate(() => { for (const [k, j] of keys.map(x => x.split(":")).sort((x, y) => y[1] - x[1])) D()[k].splice(+j, 1); });
}
function toggleSel(kind, i) {
  const key = kind + ":" + i; if (selSet.has(key)) selSet.delete(key); else selSet.add(key);
  const last = [...selSet].pop(); sel = last ? { kind: last.split(":")[0], i: +last.split(":")[1] } : null;
  selMarks(); renderRight(); tlKey = ""; updateStage();
}
/* ---- V1/A1 のクリップ(分割点・カットで区切られた区間) ---- */
function segments() {
  const b = [...new Set([0, dur(), ...D().splits, ...mergedCuts().flat(), ...gapsN().map(g => g[0])])].filter(t => t >= 0 && t <= dur()).sort((x, y) => x - y), out = [];
  for (let n = 0; n < b.length - 1; n++) if (b[n + 1] - b[n] > 1e-4 && !cutAt((b[n] + b[n + 1]) / 2)) out.push([b[n], b[n + 1]]);
  return out;
}
const segOn = (a, b, trk) => !!segSel && segSel.some(q => Math.abs(q[0] - a) < 1e-6 && Math.abs(q[1] - b) < 1e-6 && (!q[2] || !trk || q[2] === trk));
function segEl(a, b, label) {
  const trk = label ? "v" : "a", on = segOn(a, b, trk);
  const el = h("div", { class: "clip c-seg" + (label ? "" : " a") + (on ? " sel" : ""), title: `${fmtT(a)}–${fmtT(b)}  ドラッグ=移動(詰め表示) / 端ドラッグ=リップルトリム / Delete=空白を残して削除 / Shift+Delete=詰めて削除` }, label, h("i", { class: "l" }), h("i", { class: "r" }));
  el.style.left = X(a) + "px"; el.style.width = Math.max(2, X(b) - X(a)) + "px";
  el.addEventListener("mousedown", e => {
    if (e.button !== 0) return; e.stopPropagation();
    if (tool === "razor") { splitAt(snapT(timeAtX(e.clientX))); return; }
    if (e.target.classList.contains("l") || e.target.classList.contains("r")) return startSegTrim(e, e.target.classList.contains("l") ? "l" : "r", a, b);
    const one = !linked || e.altKey, ent = one ? [a, b, trk] : [a, b];  // リンク解除中 or Alt+クリック = 片方だけ
    if (e.ctrlKey || e.shiftKey) { segSel = on ? segSel.filter(q => !(Math.abs(q[0] - a) < 1e-6 && Math.abs(q[1] - b) < 1e-6 && (!q[2] || q[2] === trk))) : [...(segSel || []), ent]; if (!segSel.length) segSel = null; renderTimeline(); return; }
    if (!on || (one && segSel.some(q => !q[2]))) { select(null); segSel = [ent]; renderTimeline(); }
    if (one) return trk === "a" ? startSlip(e, a, b) : startVideoOnlyDrag(e);
    startSegMove(e, a, b);
  });
  el.addEventListener("contextmenu", e => { e.preventDefault(); if (!segOn(a, b, trk)) { select(null); segSel = [!linked || e.altKey ? [a, b, trk] : [a, b]]; renderTimeline(); } clipMenu(e); });
  return el;
}
function gapEl(at, d, v) {
  const on = !!gapSel && gapSel.some(g => Math.abs(g - at) < 1e-6);
  const el = h("div", { class: "clip c-gap" + (on ? " sel" : ""), title: `空白 ${d.toFixed(2)}秒(黒・無音)  Delete=詰める` }, v && collapsed ? "空白 " + d.toFixed(1) + "s" : "");
  if (collapsed) { el.style.left = X(at) - d * zoom + "px"; el.style.width = Math.max(2, d * zoom) + "px"; }
  else { el.style.left = X(at) - 3 + "px"; el.style.width = "6px"; }
  el.addEventListener("mousedown", e => {
    if (e.button !== 0) return; e.stopPropagation();
    if (e.ctrlKey || e.shiftKey) { gapSel = on ? gapSel.filter(g => Math.abs(g - at) > 1e-6) : [...(gapSel || []), at]; if (!gapSel.length) gapSel = null; }
    else { select(null); segSel = null; gapSel = [at]; }
    renderTimeline();
  });
  el.addEventListener("contextmenu", e => { e.preventDefault(); if (!on) { select(null); segSel = null; gapSel = [at]; renderTimeline(); } clipMenu(e); });
  return el;
}
// リンク解除: 黒(V1だけ削除)・無音(A1だけ削除)の範囲。クリックで選択、Delete で元に戻す
function trkEl(kind, i, a, b, label) {
  const key = kind + ":" + i, on = !!trkSel && trkSel.includes(key);
  const el = h("div", { class: "clip c-mute" + (on ? " sel" : "") + (kind ? "" : " ghost"), title: kind ? `${label} ${fmtT(a)}–${fmtT(b)}  Delete=元に戻す` : "音声を移動した元の場所(無音)" }, label);
  el.style.left = X(a) + "px"; el.style.width = Math.max(2, X(b) - X(a)) + "px";
  if (!kind) return el;
  el.addEventListener("mousedown", e => {
    if (e.button !== 0) return; e.stopPropagation();
    if (e.ctrlKey || e.shiftKey) { trkSel = on ? trkSel.filter(k => k !== key) : [...(trkSel || []), key]; if (!trkSel.length) trkSel = null; }
    else { select(null); segSel = null; gapSel = null; trkSel = [key]; }
    renderTimeline();
  });
  el.addEventListener("contextmenu", e => { e.preventDefault(); if (!on) { select(null); segSel = null; trkSel = [key]; renderTimeline(); } clipMenu(e); });
  return el;
}
// リンク解除して動かした音声。ドラッグでさらに移動、Delete で削除(無音になる)
function slipEl(sl, i) {
  const key = "aslip:" + i, on = !!trkSel && trkSel.includes(key), L = sl.b - sl.a;
  const el = h("div", { class: "clip c-slip" + (on ? " sel" : ""), title: `移動した音声 ${fmtT(sl.a)}–${fmtT(sl.b)} (${sl.d >= 0 ? "+" : ""}${sl.d.toFixed(2)}秒)  ドラッグ=移動 / Delete=削除` }, `♪ ${sl.d >= 0 ? "+" : ""}${sl.d.toFixed(2)}s`);
  el.style.left = (collapsed ? (outTime(sl.a) + sl.d) * zoom : X(sl.a) + sl.d * zoom) + "px"; el.style.width = Math.max(2, L * zoom) + "px";
  el.addEventListener("mousedown", e => {
    if (e.button !== 0) return; e.stopPropagation();
    if (e.ctrlKey || e.shiftKey) { trkSel = on ? trkSel.filter(k => k !== key) : [...(trkSel || []), key]; if (!trkSel.length) trkSel = null; renderTimeline(); return; }
    if (!on) { select(null); segSel = null; gapSel = null; trkSel = [key]; renderTimeline(); }
    startSlip(e, sl.a, sl.b);
  });
  el.addEventListener("contextmenu", e => { e.preventDefault(); if (!on) { select(null); segSel = null; trkSel = [key]; renderTimeline(); } clipMenu(e); });
  return el;
}
function startVideoOnlyDrag(e) {
  const x0 = e.clientX, mv = ev => { if (Math.abs(ev.clientX - x0) > 4) { toast("映像だけの移動はまだできません(音声だけなら動かせます)", 1800); up(); } };
  const up = () => { removeEventListener("mousemove", mv); removeEventListener("mouseup", up); };
  addEventListener("mousemove", mv); addEventListener("mouseup", up);
}
// A1 だけを左右にずらす(J/Lカット)。元の場所は無音、移動先の元の音は上書きで消える
function startSlip(e, a, b) {
  e.preventDefault();
  const x0 = e.clientX, L = b - a; let moved = false, sl = null, d0 = 0, raf = 0;
  const mv = ev => {
    if (!moved && Math.abs(ev.clientX - x0) < 4) return;
    if (!moved) {
      moved = true;
      if (isLocked("a1")) { toast("A1 はロック中です", 1200); return; }
      snap(); D().aslip = D().aslip || [];
      sl = D().aslip.find(x => Math.abs(x.a - a) < 1e-6 && Math.abs(x.b - b) < 1e-6);
      if (!sl) { sl = { a: +a.toFixed(3), b: +b.toFixed(3), d: 0 }; D().aslip.push(sl); }
      d0 = sl.d;
    }
    if (!sl) return;
    const o = outTime(a), tot = totalOut();
    let d = d0 + (ev.clientX - x0) / zoom;
    if (snapOn) { let bd = 8 / zoom, best = d; for (const q of [outTime(tcur), 0, tot, ...pieces().flatMap(p => [p.o, p.o + (p.gap ?? p.e - p.s)])]) for (const off of [0, L]) { const c = q - off - o; if (Math.abs(c - d) < bd) { bd = Math.abs(c - d); best = c; } } d = best; }
    sl.d = +clamp(d, -o, tot - L - o).toFixed(3); cutsCache = null;
    cancelAnimationFrame(raf); raf = requestAnimationFrame(renderTimeline);
  };
  const up = () => {
    removeEventListener("mousemove", mv); removeEventListener("mouseup", up); cancelAnimationFrame(raf);
    if (!sl) return;
    if (Math.abs(sl.d) < 1e-3) D().aslip.splice(D().aslip.indexOf(sl), 1);  // 元の位置に戻した
    const i = D().aslip.indexOf(sl); trkSel = i >= 0 ? ["aslip:" + i] : null; segSel = null; commit();
  };
  addEventListener("mousemove", mv); addEventListener("mouseup", up);
}
// V1/A1 のクリップをドラッグで移動(詰め表示)。元の場所は空白になり、移動先は上書き(重なった素材はカットされる)。素材の順番は入れ替えない
function startSegMove(e, a, b) {
  e.preventDefault();
  const x0 = e.clientX; let moved = false, base = null, raf = 0;
  const mv = ev => {
    if (!moved && Math.abs(ev.clientX - x0) < 4) return;
    if (!collapsed) { if (!moved) toast("クリップの移動は「詰め表示」で行えます", 1500); moved = true; return; }
    if (isLocked("v1") || isLocked("a1")) { if (!moved) toast("V1/A1 はロック中です", 1200); moved = true; return; }
    if (!moved) { snap(); moved = true; base = { cuts: JSON.parse(JSON.stringify(D().cuts)), gaps: JSON.parse(JSON.stringify(D().gaps || [])) }; }
    if (!base) return;
    D().cuts = JSON.parse(JSON.stringify(base.cuts)); D().gaps = JSON.parse(JSON.stringify(base.gaps)); cutsCache = null;
    const r = planMove(a, b, (ev.clientX - x0) / zoom);
    if (r) { D().cuts.push(...r.cuts); D().gaps = r.gaps; } cutsCache = null;
    cancelAnimationFrame(raf); raf = requestAnimationFrame(renderTimeline);
  };
  const up = () => { removeEventListener("mousemove", mv); removeEventListener("mouseup", up); cancelAnimationFrame(raf); if (base) { segSel = [[a, b]]; gapSel = null; commit(); } };
  addEventListener("mousemove", mv); addEventListener("mouseup", up);
}
// 移動の計算: 元の場所を空白にし、出力上 n から上書き。戻り値 = 追加カット + 新しい空白リスト
function planMove(a, b, dx) {
  const L = b - a, seq = [];
  for (const p of pieces()) {
    if (p.gap != null) { seq.push({ gap: p.gap }); continue; }
    if (p.e <= a + 1e-6 || p.s >= b - 1e-6) { seq.push({ s: p.s, e: p.e }); continue; }
    if (a - p.s > 1e-4) seq.push({ s: p.s, e: a });
    seq.push({ gap: L, me: true });
    if (p.e - b > 1e-4) seq.push({ s: b, e: p.e });
  }
  let o = 0; for (const p of seq) { p.o = o; o += p.gap ?? p.e - p.s; }
  const mi = seq.findIndex(p => p.me); if (mi < 0) return null;
  const o0 = seq[mi].o, prev = seq.slice(0, mi).reverse().find(p => p.gap == null), next = seq.slice(mi + 1).find(p => p.gap == null);
  const lo = prev ? Math.max(prev.o, prev.o + prev.e - prev.s - L) : 0, hi = next ? Math.min(next.o, next.o + next.e - next.s - L) : Infinity;
  let n = o0 + dx;
  if (snapOn) {  // 前後の素材の端・再生ヘッドに吸着
    const pts = [outTime(tcur)]; for (const p of seq) if (!p.me) pts.push(p.o, p.o + (p.gap ?? p.e - p.s));
    let bd = 8 / zoom, best = n; for (const q of pts) for (const off of [0, L]) if (Math.abs(q - off - n) < bd) { bd = Math.abs(q - off - n); best = q - off; } n = best;
  }
  n = clamp(n, lo, hi);
  const out = [], cuts = [], n1 = n + L; let placed = false;
  const place = () => { if (!placed) { out.push({ s: a, e: b }); placed = true; } };
  for (const p of seq) {
    const len = p.gap ?? p.e - p.s, pe = p.o + len;
    if (pe <= n + 1e-6) { out.push(p.me ? { gap: p.gap } : p); continue; }
    if (p.o >= n1 - 1e-6) { place(); out.push(p.me ? { gap: p.gap } : p); continue; }
    // 上書きされる範囲
    const ka = Math.max(p.o, n), kb = Math.min(pe, n1);
    if (p.gap != null) { if (ka - p.o > 1e-4) out.push({ gap: ka - p.o }); place(); if (pe - kb > 1e-4) out.push({ gap: pe - kb }); continue; }
    const sa = p.s + (ka - p.o), sb = p.s + (kb - p.o);
    if (sa - p.s > 1e-4) out.push({ s: p.s, e: sa });
    place();
    cuts.push([+sa.toFixed(3), +sb.toFixed(3)]);
    if (p.e - sb > 1e-4) out.push({ s: sb, e: p.e });
  }
  if (!placed) { if (n - o > 1e-4) out.push({ gap: n - o }); place(); }
  const gaps = []; let acc = 0;
  for (const p of out) { if (p.gap != null) acc += p.gap; else { if (acc > 1e-4) gaps.push([+p.s.toFixed(3), +acc.toFixed(3)]); acc = 0; } }
  return { cuts, gaps };  // 末尾の空白は捨てる
}
// クリップの端をドラッグ = リップルトリム(Premiere のリップル編集相当)。隣のカットが伸び縮みし、後ろが詰まる
function startSegTrim(e, side, a, b) {
  e.preventDefault(); snap();
  const cuts = D().cuts, x0 = e.clientX, edge = side === "l" ? a : b;
  let c = cuts.find(q => Math.abs((side === "l" ? Math.max(...q) : Math.min(...q)) - edge) < 1e-6);
  const fixed = c ? (side === "l" ? Math.min(...c) : Math.max(...c)) : edge;
  let raf = 0;
  const mv = ev => {
    const d = (ev.clientX - x0) / zoom;
    const v = side === "l" ? clamp(snapT(a + d), fixed, b - 0.05) : clamp(snapT(b + d), a + 0.05, fixed);
    if (Math.abs(v - fixed) < 0.005) { if (c) { cuts.splice(cuts.indexOf(c), 1); c = null; } }
    else if (c) { c[0] = +Math.min(v, fixed).toFixed(3); c[1] = +Math.max(v, fixed).toFixed(3); }
    else { c = [+Math.min(v, fixed).toFixed(3), +Math.max(v, fixed).toFixed(3)]; cuts.push(c); }
    cutsCache = null;
    cancelAnimationFrame(raf); raf = requestAnimationFrame(() => { renderTimeline(); seek(side === "l" ? v : Math.max(a, v - 1 / ((S.info && S.info.fps) || 30))); });
  };
  const up = () => { removeEventListener("mousemove", mv); removeEventListener("mouseup", up); cancelAnimationFrame(raf); segSel = null; gapSel = null; commit(); };
  addEventListener("mousemove", mv); addEventListener("mouseup", up);
}
// 再生ヘッドのドラッグ: シーク吸着(ルーラー左の🧲)がオンか Shift を押している間は、区切り線(V1/A1 の分割点・カットの継ぎ目・空白、各トラックの白い分割線)に止まる
let seekSnap = localStorage.getItem("vedit.seeksnap") === "1";
// テロップ・差し込み等のトラックの白い分割線(前のクリップの終わり = 次のクリップの頭)。[[レーン, [時刻...]], ...]
function clipSplits() {
  const byLane = new Map(), p = D();
  for (const kind of LIST) for (const it of srcItems(kind, p)) { const l = laneOf(kind, it); if (!byLane.has(l)) byLane.set(l, []); byLane.get(l).push(it); }
  return [...byLane].map(([l, its]) => {
    const st = its.map(it => +it.start).sort((x, y) => x - y), near = e => { const k = lowerIdx(st.length, j => st[j] > e - 1e-3); return k < st.length && st[k] < e + 1e-3; };  // 二分探索(差し込みが数百あると総当たりが重い)
    return [l, [...new Set(its.map(it => +it.end).filter(near))]];
  });
}
function seekPts() {
  const pts = [0, dur(), ...D().splits, ...gapsN().map(g => g[0]), ...clipSplits().flatMap(([, ts]) => ts)];
  for (const [a, b] of mergedCuts()) { pts.push(b); if (!collapsed) pts.push(a); }  // 詰めた表示では継ぎ目の後ろ(カットの終わり)へ
  return pts;
}
function scrubT(t, shift) {
  if (!(seekSnap || shift)) return t;
  let best = t, bd = 10; const xt = X(t);
  for (const q of seekPts()) { const d = Math.abs(X(q) - xt); if (d < bd) { bd = d; best = q; } }
  return best;
}
function setSeekSnap(v) { seekSnap = v; localStorage.setItem("vedit.seeksnap", v ? "1" : "0"); const b = $("#b-seeksnap"); if (b) b.classList.toggle("on", v); }
function snapT(t) { if (!snapOn) return t; let best = t, bd = 8 / zoom; for (const q of [tcur, ...edgePoints()]) if (Math.abs(q - t) < bd) { bd = Math.abs(q - t); best = q; } return best; }
// トラックの空き: クリック=選択解除 / ドラッグ=範囲選択(テロップ・トピック・差し込み)
function marquee(e) {
  const r0 = $("#inner").getBoundingClientRect(), x0 = e.clientX - r0.left, y0 = e.clientY - r0.top;
  const add = e.ctrlKey || e.shiftKey, base = add ? new Set(selSet) : new Set(), segBase = add ? segSel : null, gapBase = add ? gapSel : null, bgmBase = add && bgmSel, trkBase = add ? trkSel : null;
  if (!add) { select(null); segSel = null; gapSel = null; renderTimeline(); }
  let raf = 0;
  const box = h("div", { class: "marquee" }); $("#inner").append(box);
  const mv = ev => {
    const r = $("#inner").getBoundingClientRect(), x1 = ev.clientX - r.left, y1 = ev.clientY - r.top;
    const L = Math.min(x0, x1), Rr = Math.max(x0, x1), T = Math.min(y0, y1), B = Math.max(y0, y1);
    Object.assign(box.style, { left: L + "px", top: T + "px", width: Rr - L + "px", height: B - T + "px" });
    selSet = new Set(base); segSel = segBase ? [...segBase] : null; gapSel = gapBase ? [...gapBase] : null; bgmSel = bgmBase; trkSel = trkBase ? [...trkBase] : null;
    for (const [id, , hh, kind] of LANES) {
      if (isLocked(id) || laneTop[id] > B || laneTop[id] + laneH(hh, id) < T) continue;
      if (id === "bgm") { if (D().bgm && L <= totalOut() * zoom) bgmSel = true; continue; }
      if (id === "v1" || id === "a1") {  // 映像・音声のクリップと空白もまとめて選ぶ
        const trk = id === "v1" ? "v" : "a";
        for (const [a, b] of segments()) if (X(b) >= L && X(a) <= Rr && !segOn(a, b, linked ? null : trk)) (segSel = segSel || []).push(linked ? [a, b] : [a, b, trk]);
        const addK = k => { if (!(trkSel || []).includes(k)) (trkSel = trkSel || []).push(k); };
        if (trk === "v") (D().vmute || []).forEach((r, i) => { if (X(r[1]) >= L && X(r[0]) <= Rr) addK("vmute:" + i); });
        else { (D().amute || []).forEach((r, i) => { if (X(r[1]) >= L && X(r[0]) <= Rr) addK("amute:" + i); });
          (D().aslip || []).forEach((sl, i) => { const x = (collapsed ? outTime(sl.a) + sl.d : sl.a + sl.d) * zoom; if (x + (sl.b - sl.a) * zoom >= L && x <= Rr) addK("aslip:" + i); }); }
        for (const [at, d] of gapsN()) { const gx = collapsed ? X(at) - d * zoom : X(at); if (X(at) >= L && gx <= Rr && !(gapSel || []).includes(at)) (gapSel = gapSel || []).push(at); }
        continue;
      }
      if (!kind) continue;
      D()[kind].forEach((it, i) => { const [x0, x1] = itX(it); if (laneOf(kind, it) === id && x1 >= L && x0 <= Rr) selSet.add(kind + ":" + i); });
    }
    cancelAnimationFrame(raf); raf = requestAnimationFrame(() => { renderTimeline(); $("#inner").append(box); });  // 選ばれた物をドラッグ中から光らせる
  };
  const up = () => {
    removeEventListener("mousemove", mv); removeEventListener("mouseup", up); cancelAnimationFrame(raf); box.remove();
    const last = [...selSet].pop(); sel = last ? { kind: last.split(":")[0], i: +last.split(":")[1] } : null;
    renderTimeline(); renderRight(); tlKey = ""; updateStage();
  };
  addEventListener("mousemove", mv); addEventListener("mouseup", up);
}
/* ---- コピー/ペースト・ナッジ・右クリックメニュー ---- */
function copySel() {
  const items = [...selSet].map(k => k.split(":")).filter(([k]) => k !== "cuts").map(([k, j]) => [k, JSON.parse(JSON.stringify(D()[k][+j]))]);
  if (!items.length) return; clip = items; toast(items.length + " 個をコピーしました", 1200);
}
function pasteClip() {
  if (!clip || !clip.length) return;
  const t0 = Math.min(...clip.map(([, it]) => it.start)), added = [];
  mutate(() => {
    for (const [k, it0] of clip) { const it = JSON.parse(JSON.stringify(it0)), len = it.end - it.start; delete it.gap_at; it.start = +(tcur + it0.start - t0).toFixed(3); it.end = +Math.min(dur(), it.start + len).toFixed(3); D()[k].push(it); added.push([k, it]); }
    for (const k of LIST) D()[k].sort((a, b) => a.start - b.start);
  });
  selSet = new Set(added.map(([k, it]) => k + ":" + D()[k].indexOf(it))); const l = added[added.length - 1]; sel = { kind: l[0], i: D()[l[0]].indexOf(l[1]) };
  renderTimeline(); renderRight();
}
function nudge(frames) {
  const d = frames / ((S.info && S.info.fps) || 30), keys = [...selSet].map(k => k.split(":")).filter(([k]) => k !== "cuts");
  if (!keys.length) return;
  mutate(() => { for (const [k, j] of keys) { const it = D()[k][+j], g = isGapIt(it) && gapOf(it); if (isGapIt(it) && !g) continue; const dd = clamp(d, -it.start, (g ? g.d : dur()) - it.end); it.start = +(it.start + dd).toFixed(3); it.end = +(it.end + dd).toFixed(3); } });
}
function clipMenu(e) {
  const items = bgmSel && !segSel && !gapSel && !selSet.size ? [["BGMを外す (Delete)", () => editDelete(false)], ["BGMの設定(左パネル)", () => { leftTab = "media"; renderLeft(); }]]
    : trkSel && !segSel && !gapSel && !selSet.size ? [["削除 (Delete)", () => editDelete(false)]]
    : segSel || gapSel ? [...(segSel ? [["削除・空白を残す (Delete)", () => editDelete(false)]] : []), ["リップル削除・詰める (Shift+Delete)", () => editDelete(true)], ["再生ヘッドで分割 (Ctrl+K)", () => splitAll(tcur)]]
    : [["コピー (Ctrl+C)", copySel], ["再生ヘッドにペースト (Ctrl+V)", pasteClip], ["削除 (Delete)", deleteSel],
       ...(sel && sel.kind === "overlays" && /\.(png|jpe?g|webp)$/i.test(D().overlays[sel.i].file) ? [["画像編集で編集", imgEditSel]] : []),
       ...(sel && sel.kind === "overlays" && /\.(png|jpe?g|webp|psd)$/i.test(D().overlays[sel.i].file) ? [["Photoshop で編集", () => psOpen(D().overlays[sel.i].file)]] : []),
       ...([...selSet].some(k => k.startsWith("overlays:")) ? [["上のビデオトラックへ移動", () => moveToTrack([...selSet], 1)], ["下のビデオトラックへ移動", () => moveToTrack([...selSet], -1)]] : [])];
  showMenu(e.clientX, e.clientY, items);
}
function showMenu(x, y, items) {
  const old = $("#ctxmenu"); if (old) old.remove();
  items = items.filter((x, i, a) => x || (i > 0 && i < a.length - 1 && a[i + 1]));  // null = 区切り線
  const m = h("div", { id: "ctxmenu" }, ...items.map(x => {
    if (!x) return h("hr");
    const [label, fn] = x, d = h("div", {}, label); d.addEventListener("mousedown", ev => { ev.stopPropagation(); m.remove(); fn(); }); return d;
  }));
  document.body.append(m); m.style.left = Math.max(0, Math.min(x, innerWidth - m.offsetWidth - 4)) + "px"; m.style.top = Math.max(0, Math.min(y, innerHeight - m.offsetHeight - 4)) + "px";
  setTimeout(() => addEventListener("mousedown", () => m.remove(), { once: true }));
}
/* ---- Photoshop 連携 ---- */
const psWatch = new Map();  // file → mtime。Photoshop で保存されたら差し込みを読み直す
async function psCall(body) {
  try { const r = await api("/api/photoshop", body); if (r.path) watchFile(relPath(r.path)); return r.path; }
  catch (e) { toast("Photoshop: " + e.message, 5000); }
}
async function watchFile(f) { try { psWatch.set(f, (await api("/api/fstat?p=" + encodeURIComponent(f))).mtime); } catch (e) {} }
setInterval(async () => {
  for (const [f, m0] of psWatch) {
    try {
      const m = (await api("/api/fstat?p=" + encodeURIComponent(f))).mtime;
      if (m && m !== m0) { psWatch.set(f, m); fileVer[f] = m; tlKey = ""; updateStage(); toast("画像の保存を反映: " + f.split("/").pop(), 1800); }
    } catch (e) {}
  }
}, 1500);
const psOpen = f => psCall({ action: "open", path: f });
/* ---- 画像編集(自作の Photoshop 代わり。別窓の image.html。PNG が保存されると psWatch で差し込みを読み直す) ---- */
function openImageWin() {  // 先に窓だけ開く(待ち時間のあとだとポップアップがブロックされるため)
  return window.open("about:blank", "_blank", "popup,width=1500,height=920");
}
function showImage(w, path, isNew) {
  const url = `/image.html?f=${encodeURIComponent(path)}${isNew ? "&new=1" : ""}`;
  if (w && !w.closed) w.location.href = url; else if (!window.open(url, "_blank", "popup,width=1500,height=920")) toast("画像編集の窓を開けませんでした(ポップアップがブロックされています)。もう一度押すか、ブロックを解除してください", 7000);
}
async function imgNew(kind) {  // kind: image(透明) / telop(テロップ入り) / frame(今の画面を下敷きに)
  const w = openImageWin();
  if (kind === "frame") { await save(); pause(); toast("今の画面を原寸で書き出し中…", 6000); }
  try {
    const r = await api("/api/vimg/new", { kind, ...(kind === "frame" ? { t: outTime(tcur) } : {}) });
    addItem("overlays", { file: r.png, start: +tcur.toFixed(2), end: +Math.min(dur(), tcur + 3).toFixed(2), pos: "full", ...(vTarget > 2 ? { track: vTarget } : {}) });
    watchFile(r.png); showImage(w, r.path, true);
  } catch (e) { if (w) w.close(); toast("画像編集: " + e.message, 5000); }
}
async function imgEditSel() {
  if (!(sel && sel.kind === "overlays" && /\.(png|jpe?g|webp|vimg\.json)$/i.test(D().overlays[sel.i].file))) { toast("差し込み画像(PNG/JPG/WebP)を選択してください"); return; }
  const it = D().overlays[sel.i], w = openImageWin();
  try {
    const r = await api("/api/vimg/open", { path: it.file });  // .vimg.json があればそれ、なければ画像を1枚目のレイヤーにして作る
    if (r.png !== it.file) mutate(() => it.file = r.png);  // JPG 等は同名の PNG(画像編集の書き出し)に差し替え
    watchFile(r.png); showImage(w, r.path, r.created);
  } catch (e) { if (w) w.close(); toast("画像編集: " + e.message, 5000); }
}
function psMenu(e) {
  const items = [["新規画像を作って差し込み(V2)に置く → 画像編集で開く", () => imgNew("image")],
    ["新規テロップ画像を作る(テロップのスタイルで文字入り)", () => imgNew("telop")],
    ["今の画面を下敷きにして画像を作る(下敷きは書き出さない)", () => imgNew("frame")],
    ["選択中の差し込み画像を編集", imgEditSel],
    ["画像編集(vimg)を起動(単独の新しい窓・プロジェクトに紐づかない)", () => api("/api/vimg/launch", {}).then(() => toast("vimg を起動しています…", 2500), e => toast("vimg: " + e.message, 5000))]];
  const r = e.currentTarget.getBoundingClientRect(); showMenu(r.left, r.bottom + 2, items);
}
function cutRange(a, b) { if (isLocked("v1") && isLocked("a1")) { toast("V1/A1 はロック中です", 1200); return; } if (b - a > 0.02) mutate(() => D().cuts.push([+a.toFixed(3), +b.toFixed(3)])); }

/* ---------------- 部品: フォーム ---------------- */
function bind(el, apply, inspector = false) {
  let started = false;
  const bad = () => el.type === "number" && (el.value === "" || !isFinite(+el.value));
  el.addEventListener("input", () => { if (bad()) return; if (!started) { snap(); started = true; } apply(el.type === "checkbox" ? el.checked : el.value); commit({ inspector, keepOverlap: true }); });
  el.addEventListener("blur", () => { started = false; if (bad()) renderAll(); });  // 空欄のまま離れたら元の値に戻す
  el.addEventListener("change", () => { if (started) commit({ inspector }); started = false; });  // 入力し終えた時に重なりを詰める
  return el;
}
const rowOf = (label, ...kids) => h("div", { class: "row" }, h("label", {}, label), ...kids);
const fNum = (label, get, set, o = {}) => rowOf(label, bind(h("input", { type: "number", value: get(), step: o.step ?? 0.1, min: o.min, max: o.max }), v => set(v === "" ? 0 : +v), o.inspector), o.unit || "");
const fText = (label, get, set, o = {}) => rowOf(label, bind(h("input", { type: "text", value: get(), list: o.list }), set, o.inspector));
const fColor = (label, get, set) => rowOf(label, bind(h("input", { type: "color", value: get() }), set));
const fCheck = (label, get, set) => rowOf(label, bind(h("input", { type: "checkbox", checked: !!get() }), set));
const fSel = (label, opts, get, set, inspector = true) => {
  const s = h("select", {}, opts.map(([v, t]) => h("option", { value: v, selected: v === get() }, t)));
  return rowOf(label, bind(s, set, inspector));
};
const ANIM_T = [["none", "なし"], ["fade", "フェード"], ["pop", "ポップ"], ["slide-up", "下からスライド"]];
const ANIM_B = [["none", "なし"], ["fade", "フェード"]];
const ANIM_O = [["none", "なし"], ["fade", "フェード"], ["slide-left", "左へスライド"], ["slide-right", "右へスライド"], ["slide-up", "下へスライド"], ["slide-down", "上へスライド"]];

/* ---------------- 右: クリップ / スタイル ---------------- */
function renderRight() {
  const body = $("#right-body"); body.replaceChildren();
  for (const a of document.querySelectorAll('[data-for="right"] a')) a.classList.toggle("on", a.dataset.tab === rightTab);
  if (!S) return;
  if (rightTab === "style") return styleTab(body);
  if (rightTab === "history") return histTab(body);
  if (!sel) { body.append(h("p", { class: "note" }, "タイムラインのクリップを選ぶと、ここで編集できます。"), h("p", { class: "note" }, "ショートカットは Premiere の設定を取り込めます(vedit import-keys)。主なキー: Space 再生 / E・Ctrl+K 分割 / C 分割ツール / Q・W 前後の編集点まで削除 / Delete・Shift+Delete 選択区間を削除(詰める) / I・O・; 範囲を削除 / ↑↓ 編集点へ移動 / J・K・L シャトル / Ctrl+Z・Ctrl+Shift+Z / T テロップ・B トピック")); return; }
  const { kind, i } = sel, it = D()[kind][i]; if (!it) return;
  const tm = (label) => [
    fNum("開始", () => it.start.toFixed(2), v => { it.start = +v; }, { step: 0.05 }),
    fNum("終了", () => it.end.toFixed(2), v => { it.end = +v; }, { step: 0.05 }),
  ];
  if (kind === "cuts") {
    body.append(h("h4", {}, "カット範囲(削除される部分)"),
      fNum("開始", () => it[0].toFixed(2), v => { it[0] = +v; }, { step: 0.05 }), fNum("終了", () => it[1].toFixed(2), v => { it[1] = +v; }, { step: 0.05 }),
      h("div", { class: "row" }, h("button", { class: "danger", onclick: deleteSel }, "このカットを取り消す")));
    return;
  }
  if (kind === "telops" || kind === "topics") {
    body.append(h("h4", {}, kind === "telops" ? "テロップ" : "トピックバー"),
      bind(h("textarea", {}, it.text), v => { it.text = v; }), ...tm());
    if (kind === "telops") {
      body.append(fSel("スタイル", styleNames().map(n => [n, n]), () => it.style || "telop", v => { if (v === "telop") delete it.style; else it.style = v; }),
        fSel("出入り", ANIM_T, () => it.anim || "none", v => { it.anim = v; }),
        h("div", { class: "row" }, h("button", { onclick: () => { rightTab = "style"; styleTarget = it.style || "telop"; renderRight(); } }, "スタイルを編集…"),
          it.pos ? h("button", { onclick: () => mutate(() => delete it.pos) }, "位置を初期に戻す") : null),
        h("p", { class: "note" }, "モニター上でドラッグすると自由に動かせます。"));
    } else body.append(fSel("出入り", ANIM_B, () => it.anim || "none", v => { it.anim = v; }));
    if (it.anim && it.anim !== "none") body.append(fNum("アニメ秒", () => it.anim_dur ?? 0.2, v => { it.anim_dur = +v; }, { step: 0.05, min: 0.05 }));
    body.append(h("div", { class: "row" }, h("button", { class: "danger", onclick: deleteSel }, "削除")));
    return;
  }
  if (kind === "sfx") {
    const L = sfxFull(it);
    body.append(h("h4", {}, "効果音"), h("div", { class: "note" }, it.file), ...tm(),
      fNum("音量", () => +it.gain_db || 0, v => { it.gain_db = +v; }, { step: 0.5, unit: "dB" }),
      fNum("音の開始", () => +it.in || 0, v => { const auto = sfxAuto(it); it.in = Math.max(0, +v); if (auto) it.end = +(it.start + sfxLen(it)).toFixed(3); }, { step: 0.05, min: 0, unit: "秒(頭を飛ばす)" }),
      h("div", { class: "row" }, L != null ? h("button", { onclick: () => mutate(() => { it.end = +(it.start + sfxLen(it)).toFixed(3); }) }, "長さを音に合わせる") : null,
        h("button", { class: "danger", onclick: deleteSel }, "削除")),
      h("p", { class: "note" }, "時刻は差し込みと同じ元素材の秒。カットで消えた所の効果音は鳴りません。書き出しでは本編(処理済み)・BGM と最後にミックスします。"));
    return;
  }
  // overlays
  const posOpts = [["full", "全画面"], ["center", "中央"], ["top-left", "左上"], ["top-right", "右上"], ["bottom-left", "左下"], ["bottom-right", "右下"]];
  body.append(h("h4", {}, "映像・画像の差し込み"), h("div", { class: "note" }, it.file || "✦ " + (it.kind || "?")), ...tm(),
    fSel("配置", posOpts, () => Array.isArray(it.pos) ? "center" : (it.pos || (it.scale == null ? "full" : "top-right")), v => { it.pos = v; if (v !== "full" && it.scale == null) it.scale = 0.35; }),
    (it.pos || "full") !== "full" ? fNum("大きさ", () => it.scale ?? 0.3, v => { it.scale = +v; }, { step: 0.05, min: 0.05, max: 1, unit: "(画面幅比)" }) : fSel("はめ方", [["contain", "全体を表示"], ["cover", "画面を埋める"]], () => it.fit || "contain", v => { it.fit = v; }, false),
    fNum("素材の開始", () => it.in ?? 0, v => { it.in = +v; }, { step: 0.1, min: 0, unit: "秒(動画のみ)" }),
    fCheck("音も入れる", () => it.audio, v => { it.audio = v; }),
    fSel("入り", ANIM_O, () => it.in_anim || "none", v => { it.in_anim = v; }, false),
    fSel("出", ANIM_O, () => it.out_anim || "none", v => { it.out_anim = v; }, false),
    fNum("アニメ秒", () => it.anim_dur ?? 0.3, v => { it.anim_dur = +v; }, { step: 0.05, min: 0.05 }),
    h("div", { class: "row" }, h("button", { class: "danger", onclick: deleteSel }, "削除")));
}
function styleTab(body) {
  const names = [...styleNames(), "topic"];
  if (!names.includes(styleTarget)) styleTarget = "telop";
  const st = () => effStyle(styleTarget);
  const set = (k, v) => editStyle(styleTarget, s => { s[k] = v; });
  const sel_ = h("select", { onchange: e => { styleTarget = e.target.value; renderRight(); } }, names.map(n => h("option", { value: n, selected: n === styleTarget }, n === "topic" ? "トピックバー" : n)));
  body.append(h("div", { class: "row" }, h("label", {}, "対象"), sel_,
    h("button", { onclick: () => { const n = prompt("新しいスタイル名"); if (n && !D().styles[n] && n !== "topic") { mutate(() => { D().styles[n] = JSON.parse(JSON.stringify(effStyle("telop"))); }); styleTarget = n; renderRight(); } } }, "+ 新規")));
  const dl = h("datalist", { id: "fontlist" }, fonts.map(f => h("option", { value: f })));
  body.append(dl);
  const sample = h("div", { class: "sample" }); body.append(sample);
  const drawSample = () => { sample.replaceChildren(styleTarget === "topic" ? h("div", { style: { position: "relative", transform: "scale(.7)", transformOrigin: "left" } }, Object.assign(topicEl({ text: "トピック：サンプル" }, st(), 0.5), { style: topicEl({ text: "" }, st(), 0.5).style.cssText.replace(/left:[^;]+;top:[^;]+;/, "position:relative;") })) : telopEl({ text: "サンプルテロップ" }, st(), 0.5, true)); };
  const redo = () => { drawSample(); };
  const b2 = (el, fn) => bind(el, v => { fn(v); redo(); });
  const S2 = (label, key, o = {}) => rowOf(label, b2(h("input", { type: "number", value: st()[key], step: o.step ?? 1, min: 0 }), v => set(key, +v)));
  body.append(rowOf("フォント", b2(h("input", { type: "text", value: st().font, list: "fontlist", style: { width: "170px" } }), v => set("font", v))),
    S2("大きさ", "size"), rowOf("太字", b2(h("input", { type: "checkbox", checked: !!st().bold }), v => set("bold", v))),
    rowOf("文字色", b2(h("input", { type: "color", value: st().color }), v => set("color", v))));
  if (styleTarget === "topic") {
    body.append(rowOf("帯の色", b2(h("input", { type: "color", value: st().bar_color }), v => set("bar_color", v))),
      rowOf("アクセント色", b2(h("input", { type: "color", value: st().accent_color || "#c82828" }), v => set("accent_color", v))), S2("アクセント幅", "accent_w"),
      rowOf("帯の濃さ", b2(h("input", { type: "number", value: st().bar_alpha, step: 0.05, min: 0, max: 1 }), v => set("bar_alpha", +v))),
      S2("余白 横", "pad_x"), S2("余白 縦", "pad_y"), S2("位置 X", "x"), S2("位置 Y", "y"), S2("最小幅", "min_width"),
      h("p", { class: "note" }, "数値は 1080p 基準のピクセル。解像度に合わせて自動で拡大縮小されます。"));
  } else {
    body.append(rowOf("位置", b2(h("select", {}, [["bottom", "下"], ["center", "中央"], ["top", "上"]].map(([v, t]) => h("option", { value: v, selected: st().align === v }, t))), v => set("align", v))), S2("下/上の余白", "margin_v"));
    body.append(h("h4", {}, "縁取り(外側 → 内側)"));
    const box = h("div"); body.append(box);
    const drawOuts = () => {
      box.replaceChildren(...st().outlines.map((o, n) => h("div", { class: "row" },
        bind(h("input", { type: "color", value: o.color }), v => { editStyle(styleTarget, s => s.outlines[n].color = v); redo(); }),
        bind(h("input", { type: "number", value: o.width, step: 1, min: 0 }), v => { editStyle(styleTarget, s => s.outlines[n].width = +v); redo(); }), "px",
        h("button", { onclick: () => { mutate(() => editStyle(styleTarget, s => s.outlines.splice(n, 1))); } }, "×"))),
        h("button", { onclick: () => mutate(() => editStyle(styleTarget, s => s.outlines.push({ color: "#000000", width: 3 }))) }, "+ 縁を追加"));
    };
    drawOuts();
  }
  drawSample();
  if (D().styles[styleTarget]) body.append(h("div", { class: "row" }, h("button", { class: "danger", onclick: () => { mutate(() => { delete D().styles[styleTarget]; }); } }, styleTarget === "telop" || styleTarget === "topic" ? "初期設定に戻す" : "このスタイルを削除")));
}

/* ---------------- 左: 素材 / 音声 / カット ---------------- */
function renderLeft() {
  const body = $("#left-body"); body.replaceChildren();
  for (const a of document.querySelectorAll('[data-for="left"] a')) a.classList.toggle("on", a.dataset.tab === leftTab);
  if (!S) return;
  if (leftTab === "media") mediaTab(body); else if (leftTab === "audio") audioTab(body); else cutTab(body);
}
function mediaTab(body) {
  const i = S.info, p = D();
  body.append(h("h4", {}, "素材"), h("div", {}, p.source), h("div", { class: "note" }, i ? `${i.width}×${i.height} / ${i.fps.toFixed(2)}fps / ${fmtT(i.duration, false)}` : "見つかりません"));
  body.append(h("h4", {}, "カットのつなぎ目"),
    fSel("音", [["0", "そのまま"], ["8", "8ms フェード(推奨)"], ["20", "20ms フェード"]], () => String(p.cut_fx?.audio_fade_ms ?? 8), v => { (p.cut_fx ||= {}).audio_fade_ms = +v; }, false),
    fSel("映像", [["none", "なし"], ["dip-black", "暗転"], ["dip-white", "白飛び"]], () => p.cut_fx?.video || "none", v => { (p.cut_fx ||= {}).video = v; }, false),
    h("p", { class: "note" }, "音のブツ切れ防止は常に入れておくのがおすすめ。映像のつなぎは尺を変えません。"));
  body.append(h("h4", {}, "BGM"));
  if (!p.bgm) body.append(h("button", { onclick: async () => { const r = await api("/api/pick", { kind: "audio" }); if (r.path) mutate(() => { p.bgm = { file: relPath(r.path), volume_db: -23.5, duck: false }; setupBgm(); }); } }, "BGMを選ぶ…"));
  else body.append(h("div", { class: "note" }, p.bgm.file), fNum("音量", () => p.bgm.volume_db, v => { p.bgm.volume_db = +v; setupBgm(); }, { step: 0.5, unit: "dB" }),
    fCheck("声の間は下げる", () => p.bgm.duck, v => { p.bgm.duck = v; }), h("button", { class: "danger", onclick: () => mutate(() => { p.bgm = null; setupBgm(); }) }, "BGMを外す"));
  body.append(h("h4", {}, "書き出し"), fText("ファイル", () => p.output.file, v => { p.output.file = v; }, { inspector: false }));
}
const STEP_NAME = { declick: "De-click (リップノイズ除去)", gain: "ゲイン", normalize: "ノーマライズ", vst3: "VST3", plugin: "内蔵エフェクト" };
// 内蔵エフェクト(Pedalboard)。params: [名前, 表示名, 初期値, 単位, step]
const FX = {
  HighpassFilter: ["ローカット(ハイパス)", [["cutoff_frequency_hz", "周波数", 80, "Hz", 5]], "低いゴロゴロ・吹かれを消す"],
  NoiseGate: ["ノイズゲート", [["threshold_db", "しきい値", -45, "dB", 1], ["ratio", "比率", 4, ":1", 0.5], ["release_ms", "リリース", 150, "ms", 10]], "しきい値より小さい音(無音部の雑音)を下げる"],
  Compressor: ["コンプレッサー", [["threshold_db", "しきい値", -18, "dB", 1], ["ratio", "比率", 3, ":1", 0.5], ["attack_ms", "アタック", 5, "ms", 1], ["release_ms", "リリース", 120, "ms", 10]], "声の大小をならす"],
  LowShelfFilter: ["EQ 低域", [["cutoff_frequency_hz", "周波数", 200, "Hz", 10], ["gain_db", "増減", -3, "dB", 0.5]], ""],
  PeakFilter: ["EQ 中域(ピーク)", [["cutoff_frequency_hz", "周波数", 3000, "Hz", 50], ["gain_db", "増減", 2, "dB", 0.5], ["q", "Q(幅)", 1, "", 0.1]], ""],
  HighShelfFilter: ["EQ 高域", [["cutoff_frequency_hz", "周波数", 8000, "Hz", 100], ["gain_db", "増減", 2, "dB", 0.5]], ""],
  Limiter: ["リミッター", [["threshold_db", "しきい値", -1, "dB", 0.5], ["release_ms", "リリース", 100, "ms", 10]], "これ以上大きくしない(音割れ防止)"],
};
function audioTab(body) {
  const chain = D().audio.chain;
  body.append(h("div", { class: "row" }, h("button", { class: "primary", id: "b-audio", onclick: runAudio }, "音声処理を実行")),
    h("div", { class: "note" + (audioStale ? " warn" : ""), id: "audio-note" }, audioNoteText()));
  body.append(h("h4", {}, "音声処理(上から順に実行)"));
  chain.forEach((st, n) => {
    const hd = h("div", { class: "hd" }, h("span", {}, st.type === "plugin" ? FX[st.name]?.[0] || st.name : STEP_NAME[st.type] || st.type),
      st.type === "declick" || st.type === "normalize" ? h("span", { class: "note", title: "処理の順番は固定です" }, st.type === "declick" ? "常に最初" : "常に最後") : null,
      st.type === "declick" || st.type === "normalize" ? null : h("button", { title: "上へ", onclick: () => n > 0 && mutate(() => { chain.splice(n - 1, 0, chain.splice(n, 1)[0]); audioStale = true; }) }, "↑"),
      st.type === "declick" || st.type === "normalize" ? null : h("button", { title: "下へ", onclick: () => n < chain.length - 1 && mutate(() => { chain.splice(n + 1, 0, chain.splice(n, 1)[0]); audioStale = true; }) }, "↓"),
      h("button", { onclick: () => mutate(() => { chain.splice(n, 1); audioStale = true; }) }, "×"));
    const box = h("div", { class: "step" }, hd);
    const ast = f => v => { f(v); audioStale = true; };
    if (st.type === "declick") box.append(fNum("しきい値", () => st.params?.threshold ?? 8, ast(v => { (st.params ||= {}).threshold = +v; }), { step: 0.5, min: 2, max: 30 }), h("div", { class: "note" }, "小さいほど強く直す(雑音を多く拾う)。標準 8"));
    if (st.type === "gain") box.append(fNum("ゲイン", () => st.db, ast(v => { st.db = +v; }), { step: 0.5, unit: "dB" }));
    if (st.type === "normalize") {
      const lufs = st.lufs != null;
      box.append(rowOf("基準", h("select", { onchange: e => mutate(() => { if (e.target.value === "lufs") { st.lufs = -14; st.peak_db = -1; } else { delete st.lufs; st.peak_db = -3; } audioStale = true; }) },
        h("option", { value: "lufs", selected: lufs }, "ラウドネス (LUFS)"), h("option", { value: "peak", selected: !lufs }, "ピーク")), ""));
      if (lufs) box.append(fNum("目標", () => st.lufs, ast(v => { st.lufs = +v; }), { step: 0.5, unit: "LUFS" }), fNum("上限ピーク", () => st.peak_db, ast(v => { st.peak_db = +v; }), { step: 0.5, unit: "dB" }),
        h("div", { class: "note" }, "YouTube等は -14 / 放送は -24。上限ピークを超える分はゲインを抑えます"));
      else box.append(fNum("ピーク", () => st.peak_db, ast(v => { st.peak_db = +v; }), { step: 0.5, unit: "dB" }));
    }
    if (st.type === "vst3") box.append(h("div", { class: "note" }, st.path));
    if (st.type === "plugin") {
      const fx = FX[st.name];
      if (!fx) box.append(h("div", { class: "note" }, st.name));
      else { for (const [k, lab, def, unit, step] of fx[1]) box.append(fNum(lab, () => st.params?.[k] ?? def, ast(v => { (st.params ||= {})[k] = +v; }), { step, unit })); if (fx[2]) box.append(h("div", { class: "note" }, fx[2])); }
    }
    body.append(box);
  });
  const addBtn = (label, mk) => h("button", { onclick: () => mutate(() => { const nz = chain.findIndex(s => s.type === "normalize"); chain.splice(nz < 0 ? chain.length : nz, 0, mk()); audioStale = true; }) }, label);
  body.append(h("div", { class: "row" }, addBtn("+ De-click", () => ({ type: "declick", params: { threshold: 8 } })), addBtn("+ ゲイン", () => ({ type: "gain", db: 6 })),
    h("button", { onclick: async () => { const r = await api("/api/pick", { kind: "vst3" }); if (r.path) mutate(() => { const nz = chain.findIndex(s => s.type === "normalize"); chain.splice(nz < 0 ? chain.length : nz, 0, { type: "vst3", path: r.path, params: {} }); audioStale = true; }); } }, "+ VST3…"),
    !chain.some(s => s.type === "normalize") ? addBtn("+ ノーマライズ", () => ({ type: "normalize", lufs: -14, peak_db: -1 })) : null),
    h("div", { class: "row" }, h("select", { onchange: e => { const n = e.target.value; e.target.value = ""; if (!n) return; mutate(() => { const nz = chain.findIndex(s => s.type === "normalize"); chain.splice(nz < 0 ? chain.length : nz, 0, { type: "plugin", name: n, params: Object.fromEntries(FX[n][1].map(q => [q[0], q[2]])) }); audioStale = true; }); } },
      h("option", { value: "" }, "+ エフェクトを追加…"), ...Object.entries(FX).map(([k, v]) => h("option", { value: k }, v[0])))),
    h("div", { class: "row" }, h("button", { title: "ローカット→ゲート→コンプ→リミッター→-14LUFS の声向けセット", onclick: () => mutate(() => {
      const keep = chain.filter(s => s.type === "declick" || s.type === "vst3");
      const fx = n => ({ type: "plugin", name: n, params: Object.fromEntries(FX[n][1].map(q => [q[0], q[2]])) });
      chain.splice(0, chain.length, ...keep, fx("HighpassFilter"), fx("NoiseGate"), fx("Compressor"), fx("Limiter"), { type: "normalize", lufs: -14, peak_db: -1 }); audioStale = true; }) }, "声のおすすめ設定")));
}
async function runAudio() {
  await save(); const b = $("#b-audio") || {}, w = $("#b-rawwarn"); b.disabled = w.disabled = true; b.textContent = "処理中…"; w.textContent = "⏳ 音声処理中…(終わるまで元の音)";
  try { const r = await runJob({ kind: "audio", force: true }); audioVer++; toast(`音声処理が完了(クリック修復 ${r.clicks_repaired ?? 0}件)`); await load(false); }
  catch (e) { toast("失敗: " + e.message, 6000); }
  w.disabled = false; updAudioWarn(); renderLeft();
}
let silenceResult = null;
function cutTab(body) {
  const p = D(), q = (S.silence ||= { min: 0.7, noise: -40, keep_before: 0.2, keep_after: 0.35 });
  body.append(h("h4", {}, "無音カット"),
    fNum("無音の長さ", () => q.min, v => { q.min = +v; }, { step: 0.1, unit: "秒以上", inspector: false }), fNum("無音の基準", () => q.noise, v => { q.noise = +v; }, { step: 1, unit: "dB以下", inspector: false }),
    fNum("頭に残す", () => q.keep_before, v => { q.keep_before = +v; }, { step: 0.05, unit: "秒", inspector: false }), fNum("後ろに残す", () => q.keep_after, v => { q.keep_after = +v; }, { step: 0.05, unit: "秒", inspector: false }));
  body.append(h("div", { class: "row" }, h("button", { id: "b-sil", onclick: async () => {
    await save(); const b = $("#b-sil"); b.disabled = true; b.textContent = "検出中…";
    try { silenceResult = await runJob({ kind: "silence", ...q }); } catch (e) { toast("失敗: " + e.message, 6000); }
    renderLeft();
  } }, "無音を検出")));
  if (silenceResult) {
    const rem = silenceResult.cuts.reduce((s, [a, b]) => s + b - a, 0);
    body.append(h("div", { class: "note" }, `${silenceResult.cuts.length}か所 / 約${rem.toFixed(1)}秒を削除`),
      h("div", { class: "row" }, h("button", { class: "primary", onclick: () => { const cs = silenceResult.cuts; silenceResult = null; mutate(() => { const all = [...D().cuts, ...cs].map(c => [+c[0], +c[1]]).sort((x, y) => x[0] - y[0]), m = []; for (const c of all) { const l = m[m.length - 1]; if (l && c[0] <= l[1] + 1e-3) l[1] = Math.max(l[1], c[1]); else m.push(c); } D().cuts = m; /* 重なり・重複はまとめる */ }); } }, "カットに追加"), h("button", { onclick: () => { silenceResult = null; renderLeft(); } }, "破棄")));
  }
  body.append(h("h4", {}, `カット一覧 (${p.cuts.length})`));
  const merged = mergedCuts(), removed = merged.reduce((s, [a, b]) => s + b - a, 0);
  body.append(h("div", { class: "note" }, `合計 ${removed.toFixed(1)}秒 削除 → 仕上がり ${fmtT(totalOut(), false)}`));
  const list = h("div", { style: { maxHeight: "260px", overflow: "auto" } });
  [...p.cuts.entries()].sort((a, b) => a[1][0] - b[1][0]).slice(0, 300).forEach(([i, c]) => list.append(h("div", { class: "row", style: { cursor: "pointer", margin: "2px 0" }, onclick: () => { select("cuts", i); seek(c[0]); } }, `${fmtT(c[0])} – ${fmtT(c[1])}`, h("span", { class: "dim" }, `(${(c[1] - c[0]).toFixed(2)}s)`))));
  body.append(list);
  if (p.cuts.length) body.append(h("div", { class: "row" }, h("button", { class: "danger", onclick: () => confirm("カットをすべて取り消しますか?") && mutate(() => { D().cuts = []; }) }, "すべて取り消す")));
}
document.addEventListener("click", e => {
  const a = e.target.closest(".tabs a"); if (!a) return;
  if (a.parentElement.dataset.for === "left") { leftTab = a.dataset.tab; renderLeft(); } else { rightTab = a.dataset.tab; renderRight(); }
});

/* ---------------- 書き出し・確認 ---------------- */
function modal(content) { $("#modalbox").replaceChildren(...content); $("#modal").hidden = false; }
const closeModal = () => $("#modal").hidden = true;
// 書き出し設定(Premiere の「書き出し設定」相当)。設定は project.output に保存される
function exportDialog() {
  pause();
  const o = D().output, row = (label, el) => h("div", { class: "row" }, h("label", { style: { width: "110px" } }, label), el);
  const sel = (key, opts, def) => { const e = h("select", {}, ...opts.map(([v, l]) => h("option", { value: v }, l))); e.value = String(o[key] ?? def); return e; };
  const file = h("input", { type: "text", value: o.file || "out/final.mp4", style: { flex: 1 } });
  const res = sel("height", [["", "元のまま (" + (S.info ? S.info.height + "p" : "") + ")"], ["2160", "2160p (4K)"], ["1440", "1440p"], ["1080", "1080p"], ["720", "720p"]], "");
  const fps = sel("fps", [["", "元のまま (" + (S.info ? (+S.info.fps).toFixed(2) : "") + ")"], ["60", "60"], ["30", "30"], ["29.97", "29.97"], ["24", "24"]], "");
  const codec = sel("codec", [["h264", "H.264 (互換性重視・YouTube 向け)"], ["h265", "H.265 / HEVC (小さいファイル)"]], "h264");
  const enc = sel("encoder", [["auto", "自動 (GPU があれば GPU)"], ["gpu", "GPU (NVENC)"], ["cpu", "CPU (x264/x265・高品質だが遅い)"]], "auto");
  const accel = sel("gpu_accel", [["true", "オン (差し込み・画像の重ね合わせを GPU で)"], ["false", "オフ (すべて CPU)"]], "true");
  const eng = sel("engine", [["ffmpeg", "ffmpeg (標準)"], ["vedit", "自前エンジン (GPU 合成・失敗したら ffmpeg でやり直し)"]], "ffmpeg");
  const q = sel("quality", [["high", "高画質"], ["standard", "標準"], ["small", "軽量 (小さいファイル)"]], "high");
  const br = h("input", { type: "number", min: 0, step: 1, value: o.bitrate_mbps || "", placeholder: "空欄 = 画質優先", style: { width: "110px" } });
  const ab = sel("audio_kbps", [["128", "128 kbps"], ["192", "192 kbps"], ["256", "256 kbps"], ["320", "320 kbps"]], 192);
  const cpu = sel("cpu_percent", [["25", "25% (軽い・遅い)"], ["50", "50% (おすすめ・PC を使いながら)"], ["75", "75%"], ["95", "95% 放置用 (最速・書き出し中は PC が重い)"]], 50);
  const hasIO = markIn != null && markOut != null && markOut > markIn;
  const rng = h("select", {}, h("option", { value: "all" }, "シーケンス全体"), ...(hasIO ? [h("option", { value: "io" }, `イン〜アウト (${fmtT(markIn)}–${fmtT(markOut)})`)] : []));
  const openAfter = h("input", { type: "checkbox", checked: localStorage.getItem("vedit.openAfter") !== "0" });
  const go = async draft => {
    mutate(() => {
      o.file = file.value.trim() || "out/final.mp4";
      for (const [k, el] of [["height", res], ["fps", fps], ["audio_kbps", ab], ["bitrate_mbps", br], ["cpu_percent", cpu]]) { if (el.value === "") delete o[k]; else o[k] = +el.value; }
      for (const [k, el] of [["codec", codec], ["encoder", enc], ["quality", q], ["engine", eng]]) o[k] = el.value;
      o.gpu_accel = accel.value === "true";
      if (o.fps == null) o.fps = null;
    });
    localStorage.setItem("vedit.openAfter", openAfter.checked ? "1" : "0");
    doExport(draft, rng.value === "io" ? [outTime(markIn), outTime(markOut)] : null, openAfter.checked);
  };
  const browse = h("button", { onclick: async () => { const r = await api("/api/pick", { kind: "save", name: file.value }); if (r.path) file.value = relPath(r.path); } }, "参照…");
  modal([h("h3", {}, "書き出し設定"),
    row("ファイル", h("div", { class: "row", style: { flex: 1 } }, file, browse)),
    row("範囲", rng), row("形式", codec), row("解像度", res), row("フレームレート", fps),
    row("エンコーダー", enc), row("GPU 合成", accel), row("書き出しエンジン", eng), row("画質", q), row("ビットレート", h("span", {}, br, " Mbps (YouTube 1080p の目安 8〜12)")), row("音声", ab), row("CPU の使用", h("span", {}, cpu, " 書き出しに使うコアの割合")),
    h("label", { class: "row" }, openAfter, "書き出し後にフォルダを開く"),
    h("div", { class: "row", style: { justifyContent: "flex-end", marginTop: "10px" } },
      h("button", { onclick: closeModal }, "キャンセル"), h("button", { onclick: () => go(true) }, "低画質で試し書き出し"), h("button", { class: "primary", onclick: () => go(false) }, "書き出し"))]);
}
async function doExport(draft, range = null, openAfter = false) {
  await save(); pause();
  const bar = h("div", {}), txt = h("div", {}, "準備中…（音声処理が必要なら先に行います）");
  modal([h("h3", {}, draft ? "低画質で書き出し" : "書き出し"), txt, h("div", { class: "bar" }, bar)]);
  try {
    const r = await runJob({ kind: "render", draft, ...(range ? { range } : {}) }, p => { bar.style.width = p * 100 + "%"; txt.textContent = `書き出し中… ${Math.round(p * 100)}%`; });
    const ck = r.checks || [], au = r.audio;
    modal([h("h3", {}, "完了"), h("div", {}, r.output), h("div", { class: "note" }, `${fmtT(r.duration, false)} / ${r.segments}区間` + (au ? ` / 音量 ${au.lufs} LUFS・ピーク ${au.true_peak} dBTP` : "")),
      ck.length ? h("div", { class: "note warn", style: { maxHeight: "180px", overflow: "auto", userSelect: "text" } }, `⚠ 確認してください(${ck.length}件)`,
        ...ck.map(c => h("div", { style: c.at != null ? { cursor: "pointer", textDecoration: "underline dotted" } : {}, onclick: () => { if (c.at != null) { closeModal(); seek(srcAt(clamp(c.at, 0, totalOut())).t); } } }, (c.at != null ? fmtT(c.at, false) + " " : "") + c.msg)))
        : h("div", { class: "note" }, "✔ チェック: 効果音の抜け・重なり・音量 問題なし"),
      h("div", { class: "row" }, h("button", { class: "primary", onclick: () => { api("/api/open", { path: r.output }); closeModal(); } }, "フォルダを開く"), h("button", { onclick: closeModal }, "閉じる"))]);
    if (openAfter) api("/api/open", { path: r.output });
    await load(false);
  } catch (e) { modal([h("h3", {}, "書き出しに失敗"), h("pre", { style: { whiteSpace: "pre-wrap", userSelect: "text", maxHeight: "260px", overflow: "auto" } }, e.message), h("button", { onclick: closeModal }, "閉じる")]); }
}
async function checkFrame() {
  await save(); pause(); toast("実際の描画で確認中…", 8000);
  try { const r = await runJob({ kind: "frame", t: outTime(tcur) }); const box = $("#checkimg"); $("img", box).src = mediaUrl(r.output, Date.now()); box.hidden = false; $("#toast").hidden = true; }
  catch (e) { toast("失敗: " + e.message, 6000); }
}
$("#checkimg button").onclick = () => $("#checkimg").hidden = true;

/* ---------------- イベント ---------------- */
$("#b-play").onclick = () => playing ? pause() : play();
$("#b-start").onclick = () => seek(0); $("#b-end").onclick = () => seek(dur());
$("#b-saveas").onclick = saveAs; $("#b-undo").onclick = () => doUndo(undoS, redoS); $("#b-redo").onclick = () => doUndo(redoS, undoS);
$("#b-export").onclick = exportDialog; $("#b-draft").onclick = () => doExport(true); $("#b-check").onclick = checkFrame;
$("#t-tool").onclick = () => setTool(tool === "cut" ? "select" : "cut");
$("#t-razor").onclick = () => setTool(tool === "razor" ? "select" : "razor");
$("#t-in").onclick = () => { markIn = tcur; markOut = null; renderTimeline(); };
$("#t-out").onclick = () => { if (markIn != null) { markOut = tcur; renderTimeline(); } };
$("#t-x").onclick = () => { if (markIn != null && markOut != null) { const a = Math.min(markIn, markOut), b = Math.max(markIn, markOut); markIn = markOut = null; cutRange(a, b); } else toast("イン(I)とアウト(O)を打ってください"); };
$("#t-addt").onclick = addTelop; $("#t-addb").onclick = addTopic; $("#t-addo").onclick = addOverlay; $("#t-adds").onclick = addSfx;
$("#vol").value = localStorage.getItem("vedit.vol") ?? 80;
$("#vol").oninput = () => { muted = false; applyVol(); };
$("#b-mute").onclick = () => { muted = !muted; applyVol(); };
applyVol();
$("#b-rawwarn").onclick = () => { if (confirm("音声処理を実行して、書き出しと同じ音で再生できるようにしますか?(長い動画は10分ほどかかります)")) runAudio(); };
$("#c-proc").onchange = () => { updAudioWarn(); if (playing) { pause(); play(); } renderTimeline(); };
addEventListener("resize", () => { if (S) { layoutStage(); renderTimeline(); } });
/* ---------------- 修正点(review.js)。右の4列目に出すか、別窓(review.html)に出す ---------------- */
const rvChan = Review.chan;
let rvWin = null, rvPop = false;
const rvPos = () => S ? { src: tcur, out: curOut(), playing: playing && shuttle >= 0, rate, gap: gapHold ? { at: gapHold.at, off: gapHoldPos() } : gpos ? { at: gpos.at, off: gpos.off } : null } : null;
// 修正点のクリック: 元素材の src へ(空白の中なら空白の頭)。タイムラインもそこを見せる
function rvSeek(src, gapAt) {
  if (!S) return;
  if (playing) pause();
  if (gapAt != null) { const p = pieces().find(p => p.gap != null && Math.abs(p.at - gapAt) < 1e-3); seek(gapAt); if (p) { gpos = { at: p.at, d: p.gap, off: 0 }; updatePlayhead(); updateTracks(); updateStage(); } }
  else seek(src);
  const sc = $("#scroller"); sc.scrollLeft = Math.max(0, X(tcur) - sc.clientWidth / 2);
}
function rvShow(on) {
  $("#review").hidden = !on; $("main").classList.toggle("rv", on); $("#b-review").classList.toggle("on", on || rvPop);
  localStorage.setItem("vedit.review", on ? "1" : "0");
  if (on) Review.mount($("#review"), { docked: true, pos: rvPos, seek: rvSeek, popout: rvPopout, close: () => rvShow(false) });
  else Review.unmount();
  dispatchEvent(new Event("resize"));
}
function rvPopout() {
  rvShow(false); rvPop = true; $("#b-review").classList.add("on");
  if (rvWin && !rvWin.closed) return rvWin.focus();
  rvWin = window.open(`/review.html?t=${TOKEN}`, "vedit-review-" + TOKEN, "popup,width=760,height=900");
  if (!rvWin) { rvPop = false; toast("別窓を開けませんでした(ポップアップがブロックされています)", 6000); rvShow(true); }
}
$("#b-review").onclick = () => {
  if (rvPop && rvWin && !rvWin.closed) return rvWin.focus();
  rvShow($("#review").hidden);
};
if (rvChan) {
  rvChan.onmessage = e => {
    const m = e.data;
    if (m.type === "seek") rvSeek(m.src, m.gapAt);
    else if (m.type === "hello") { rvPop = true; $("#b-review").classList.add("on"); }
    else if (m.type === "closed") { rvPop = false; rvWin = null; $("#b-review").classList.toggle("on", !$("#review").hidden); }
    else if (m.type === "dock") { rvPop = false; rvWin = null; rvShow(true); }
  };
  setInterval(() => { if (rvPop) { const p = rvPos(); if (p) rvChan.postMessage({ type: "pos", ...p }); } }, 100);
  addEventListener("beforeunload", () => rvChan.postMessage({ type: "bye" }));
}
/* ---------------- キーマップ(Premiere のショートカットを vedit import-keys で取り込める) ---------------- */
let keymap = {}, keyIndex = {};
async function loadKeymap() {
  try { keymap = await api("/api/keymap"); } catch (e) { keymap = {}; }
  keyIndex = {};
  for (const [act, ks] of Object.entries(keymap)) for (const k of ks) keyIndex[k.toLowerCase()] = act;
  keyIndex["ctrl+y"] ??= "redo";
  // ボタンのヒントに実際のキーを出す(キーマップを変えても食い違わない)
  const BTN = { "b-undo": "undo", "b-redo": "redo", "b-start": "home", "b-play": "play", "b-end": "end", "b-mute": "mute", "t-razor": "razorTool", "t-snap": "snapToggle",
    "t-tool": "cutTool", "t-in": "markIn", "t-out": "markOut", "t-x": "cutRange", "t-addt": "addTelop", "t-addb": "addTopic", "t-link": "linkToggle", "b-saveas": "saveAs" };
  for (const [id, act] of Object.entries(BTN)) {
    const el = document.getElementById(id); if (!el) continue;
    const base = el.dataset.tip ??= (el.title || "").replace(/^[^:]{1,20} : /, "").replace(/^(Ctrl\+\w+|Home|End|Space|[A-Z])$/, "");
    const ks = (keymap[act] || []).join(" / ");
    el.title = ks ? (base ? `${ks} : ${base}` : ks) : base;
  }
}
const comboOf = e => {
  const names = { " ": "Space", ArrowLeft: "Left", ArrowRight: "Right", ArrowUp: "Up", ArrowDown: "Down" };
  const k = names[e.key] || (e.key.length === 1 ? e.key.toUpperCase() : e.key);
  const m = (e.ctrlKey ? "Ctrl+" : "") + (e.altKey ? "Alt+" : "") + ((e.shiftKey && (k.length > 1 || /[A-Z]/.test(k))) ? "Shift+" : "");
  return (m + k).toLowerCase();
};
const edgePoints = () => {
  const p = D(), pts = new Set([0, dur(), ...p.splits]);
  for (const c of mergedCuts()) { pts.add(c[0]); pts.add(c[1]); }
  for (const k of LIST) for (const it of srcItems(k, p)) { pts.add(it.start); pts.add(it.end); }
  return [...pts].filter(t => t >= 0 && t <= dur()).sort((a, b) => a - b);
};
function segmentAt(t) {
  const bounds = new Set([0, dur(), ...D().splits]); for (const c of mergedCuts()) { bounds.add(c[0]); bounds.add(c[1]); }
  const b = [...bounds].sort((x, y) => x - y); let a0 = 0, b0 = dur();
  for (let n = 0; n < b.length; n++) if (b[n] <= t) a0 = b[n]; else { b0 = b[n]; break; }
  return [a0, b0];
}
function splitAt(t) {
  if (isLocked("v1") && isLocked("a1")) { toast("V1/A1 はロック中です", 1200); return; }
  t = +clamp(t, 0, dur()).toFixed(3);
  if (cutAt(t) || D().splits.some(s => Math.abs(s - t) < 0.02) || t <= 0.02 || t >= dur() - 0.02) return;
  mutate(() => { D().splits.push(t); D().splits.sort((a, b) => a - b); });
}
// E / Ctrl+K: ロックしていない全トラックを再生ヘッドで分割(V1/A1 は分割点、テロップ・トピック・差し込みは2つに分ける)
function splitAll(t) {
  t = +clamp(t, 0, dur()).toFixed(3);
  const av = !(isLocked("v1") && isLocked("a1")) && !cutAt(t) && t > 0.02 && t < dur() - 0.02 && !D().splits.some(s => Math.abs(s - t) < 0.02);
  const hits = LIST.flatMap(k => srcItems(k).filter(it => !isLocked(laneOf(k, it)) && +it.start < t - 0.02 && +it.end > t + 0.02).map(it => [k, it]));
  if (!av && !hits.length) { if (LANES.every(([id]) => isLocked(id) || id === "bgm")) toast("すべてロック中です", 1200); return; }
  mutate(() => {
    if (av) { D().splits.push(t); D().splits.sort((a, b) => a - b); }
    for (const [k, it] of hits) {
      const b = JSON.parse(JSON.stringify(it));
      b.start = t; it.end = t;
      if (k === "sfx" || k === "overlays" && !!it.file && !/\.(png|jpe?g|webp|bmp|gif)$/i.test(it.file)) b.in = +((+(it.in || 0)) + inDelta(it, +it.start, t)).toFixed(3);
      if (k === "overlays") { delete b.in_anim; delete it.out_anim; }
      D()[k].push(b);
    }
    for (const k of LIST) D()[k].sort((a, b) => a.start - b.start);
  });
  select(null);
}
// 選択中の V1/A1 クリップ・空白・テロップ等をまとめて削除。ripple=false なら V1/A1 は空白を残す(リフト)、true なら詰める
function editDelete(ripple) {
  const gaps = gapSel || [], keys = selSet.size ? [...selSet] : sel ? [sel.kind + ":" + sel.i] : [], dropBgm = bgmSel && !!D().bgm, tk = trkSel || [];
  // V1/A1 の選択: 両方選ばれた区間はリンク扱い、片方だけなら 黒/無音
  const both = [], vOnly = [], aOnly = [];
  for (const q of segSel || []) {
    if (both.concat(vOnly, aOnly).some(x => Math.abs(x[0] - q[0]) < 1e-6 && Math.abs(x[1] - q[1]) < 1e-6)) continue;
    const same = (segSel || []).filter(x => Math.abs(x[0] - q[0]) < 1e-6 && Math.abs(x[1] - q[1]) < 1e-6);
    const v = same.some(x => !x[2] || x[2] === "v"), a = same.some(x => !x[2] || x[2] === "a");
    (v && a ? both : v ? vOnly : aOnly).push([q[0], q[1]]);
  }
  const segs = both;
  if (!segs.length && !vOnly.length && !aOnly.length && !gaps.length && !keys.length && !dropBgm && !tk.length) return false;
  if (ripple && (vOnly.length || aOnly.length)) toast("片方だけの削除は詰められないので、黒/無音にしました", 1800);
  if ((segs.length || gaps.length) && isLocked("v1") && isLocked("a1")) { toast("V1/A1 はロック中です", 1200); return true; }
  segSel = null; gapSel = null; bgmSel = false; trkSel = null; sel = null; selSet = new Set();
  mutate(() => {
    const p = D(); p.vmute = p.vmute || []; p.amute = p.amute || []; p.aslip = p.aslip || [];
    const tkn = tk.map(k => k.split(":")).sort((x, y) => y[1] - x[1]);
    for (const [k, j] of tkn) { const it = p[k][+j]; if (k === "aslip") p.amute.push([it.a, it.b]); }  // 動かした音を消す = 無音に
    for (const [k, j] of tkn) p[k].splice(+j, 1);  // 黒/無音を消す = 元に戻す
    for (const [a, b] of vOnly) p.vmute.push([+a.toFixed(3), +b.toFixed(3)]);
    for (const [a, b] of aOnly) { p.amute.push([+a.toFixed(3), +b.toFixed(3)]); p.aslip = p.aslip.filter(x => !(Math.abs(x.a - a) < 1e-6 && Math.abs(x.b - b) < 1e-6)); }
    if (dropBgm) { D().bgm = null; setupBgm(); }
    for (const [k, j] of keys.map(x => x.split(":")).sort((x, y) => y[1] - x[1])) D()[k].splice(+j, 1);
    cutsCache = null;
    D().gaps = gapsN().filter(([at]) => !gaps.some(x => Math.abs(x - at) < 1e-6)).map(g => [...g]);  // 選んだ空白は詰める
    for (const [a, b] of segs) { D().cuts.push([+a.toFixed(3), +b.toFixed(3)]); if (!ripple && b < dur() - 1e-3) D().gaps.push([+b.toFixed(3), +(b - a).toFixed(3)]); }
    normGaps();
  });
  return true;
}
const rippleCut = () => editDelete(true);
// 再生ヘッドから前/後ろのいちばん近い区切り(分割点・カットの端・空白・ロックしていないトラックのクリップの始点/終点)までを削除(Q/W)。
// 近さはカット後の時刻で比べる(カットの両端のように同じ位置に見える点を飛ばす)。ヘッドは繋ぎ目に残る
function trimPoints() {
  const p = D(), pts = new Set([0, dur(), ...p.splits, ...mergedCuts().flat(), ...gapsN().map(g => g[0])]);
  for (const k of LIST) for (const it of srcItems(k, p)) if (!isLocked(laneOf(k, it))) { pts.add(+it.start); pts.add(+it.end); }
  return [...pts].filter(t => t >= 0 && t <= dur());
}
function trimTo(dir) {
  const t = tcur, o = outTime(t), pts = trimPoints().map(x => [x, outTime(x)]);
  if (dir < 0) {
    let a = null; for (const [x, ox] of pts) if (ox < o - 0.02 && (!a || ox > a[1] || (ox === a[1] && x > a[0]))) a = [x, ox];
    if (a) { cutRange(a[0], t); seek(a[0]); }
  } else {
    let b = null; for (const [x, ox] of pts) if (ox > o + 0.02 && (!b || ox < b[1] || (ox === b[1] && x < b[0]))) b = [x, ox];
    if (b) cutRange(t, b[0]);
  }
}
function setCollapsed(v) { collapsed = v; $("#t-collapse").classList.toggle("on", v); const t = tcur; renderTimeline(); $("#scroller").scrollLeft = Math.max(0, X(t) - 200); drawRuler(); drawWave(); }
$("#t-collapse").addEventListener("click", () => setCollapsed(!collapsed));
// リンク(映像と音声を一緒に選ぶ)。解除すると V1/A1 を別々に選んで、黒/無音・音声だけ移動ができる。Alt+クリックでも片方だけ選べる
const linkBtn = h("button", { id: "t-link", title: "映像と音声のリンク(オフ=別々に選択・削除・音声だけ移動)。Alt+クリックで一時的に片方だけ選択" });
function setLinked(v) { linked = v; localStorage.setItem("vedit.linked", v ? "1" : "0"); linkBtn.textContent = v ? "🔗 リンク" : "⛓️‍💥 リンク解除中"; linkBtn.classList.toggle("on", v); if (S) renderTimeline(); }
linkBtn.addEventListener("click", () => setLinked(!linked)); $("#t-snap").after(linkBtn); setLinked(linked);
$("#t-snap").addEventListener("click", () => ACTIONS.snapToggle());
$("#b-ps").addEventListener("click", psMenu);
const setTool = n => { tool = n; cutTool = n === "cut"; $("#t-razor").classList.toggle("on", n === "razor"); $("#t-tool").classList.toggle("on", cutTool); $("#scroller").style.cursor = n === "razor" ? "crosshair" : ""; };
function setRate(r) { rate = r; vid.playbackRate = r; vidB.playbackRate = r; aud.playbackRate = r; bgm.playbackRate = r; pva.playbackRate = r; for (const L of sfxLive.values()) L.src.playbackRate.value = r; }
const ACTIONS = {
  play: () => { setRate(1); if (shuttle) { shuttle = 0; seek(tcur); return; } playing ? pause() : play(); },
  frameBack: () => seek(tcur - 1 / (S.info.fps || 30)), frameFwd: () => seek(tcur + 1 / (S.info.fps || 30)),
  back5: () => seek(tcur - 5 / (S.info.fps || 30)), fwd5: () => seek(tcur + 5 / (S.info.fps || 30)),
  prevEdit: () => { const a = [...edgePoints()].reverse().find(x => x < tcur - 0.02); if (a != null) seek(a); },
  nextEdit: () => { const b = edgePoints().find(x => x > tcur + 0.02); if (b != null) seek(b); },
  home: () => seek(0), end: () => seek(dur()),
  markIn: () => $("#t-in").click(), markOut: () => $("#t-out").click(),
  gotoIn: () => markIn != null && seek(markIn), gotoOut: () => markOut != null && seek(markOut),
  extract: () => { if (markIn != null && markOut != null) $("#t-x").click(); else rippleCut(); }, cutRange: () => $("#t-x").click(),
  rippleTrimPrev: () => trimTo(-1), rippleTrimNext: () => trimTo(1),
  split: () => splitAll(tcur), razorTool: () => setTool(tool === "razor" ? "select" : "razor"),
  selectTool: () => setTool("select"), cutTool: () => setTool(tool === "cut" ? "select" : "cut"),
  delete: () => { if (!editDelete(false) && markIn != null && markOut != null) $("#t-x").click(); }, rippleDelete: () => editDelete(true),
  undo: () => doUndo(undoS, redoS), redo: () => doUndo(redoS, undoS), save: () => save(), saveAs,
  zoomOut: () => setZoom(zoom / 1.5), zoomIn: () => setZoom(zoom * 1.5),
  zoomFit: () => setZoom(clamp(($("#scroller").clientWidth - 40) / tlen(), 0.5, 300)),
  // J/K/L: Premiere 同様。押すたびに倍速(1→2→4→8)、J は逆再生、K で停止
  shuttleFwd: () => { if (shuttle < 0) { shuttle = 0; seek(tcur); } if (!playing) { setRate(1); play(); } else setRate(Math.min(8, rate * 2)); },
  shuttleBack: () => { if (playing) pause(); shuttle = shuttle < 0 ? Math.max(-8, shuttle * 2) : -1; },
  shuttleStop: () => { shuttle = 0; pause(); setRate(1); seek(tcur); },
  addTelop, addTopic, mute: () => $("#b-mute").click(),
  snapToggle: () => { snapOn = !snapOn; $("#t-snap").classList.toggle("on", snapOn); toast("スナップ " + (snapOn ? "オン" : "オフ"), 900); },
  nudgeLeft: () => nudge(-1), nudgeRight: () => nudge(1), nudgeLeft5: () => nudge(-5), nudgeRight5: () => nudge(5),
  copy: copySel, paste: pasteClip,
  selectAll: () => { selSet = new Set(LIST.flatMap(k => D()[k].map((it, i) => [k, it, i]).filter(([, it]) => !isLocked(laneOf(k, it))).map(([, , i]) => k + ":" + i))); renderTimeline(); },
  deselectAll: () => { select(null); segSel = null; gapSel = null; bgmSel = false; trkSel = null; renderTimeline(); },
  linkToggle: () => setLinked(!linked),
};
addEventListener("keydown", e => {
  const ae = document.activeElement, typing = /^(TEXTAREA|SELECT)$/.test(ae.tagName) || ae.isContentEditable || ae.tagName === "INPUT" && !/^(range|checkbox|radio|button|color)$/.test(ae.type);
  const act = keyIndex[comboOf(e)];
  if (!act || !S) return;
  if (typing && act !== "save" && act !== "saveAs") return;
  e.preventDefault();
  ACTIONS[act]?.();
});
loadKeymap();


function renderAll(inspector = true) {
  if (!S) return;
  renderTimeline(); if (inspector) { renderRight(); renderLeft(); } else { if (leftTab === "cut") renderLeft(); if (rightTab === "history") renderRight(); }
  setupBgm(); tlKey = ""; updateStage();
}
load(true).then(() => { if (localStorage.getItem("vedit.review") === "1") rvShow(true); layoutStage(); renderTimeline(); requestAnimationFrame(tick); }).catch(e => { document.body.append(h("pre", {}, "起動に失敗: " + e.message)); });
$("#meter").addEventListener("click", () => { peakHold = -Infinity; });
// 右上: CPU / GPU / メモリの使用率(2秒ごと。窓が隠れている間は聞かない)
async function sysTick() {
  if (!document.hidden) try {
    const v = await api("/api/sys"), el = $("#sysmon"), cls = x => x >= 95 ? "hot" : x >= 80 ? "warm" : "";
    const it = (k, x, txt) => x == null ? null : h("span", { class: cls(x) }, h("b", {}, k), txt ?? Math.round(x) + "%");
    el.replaceChildren(...[it("CPU", v.cpu), it("GPU", v.gpu), it("VRAM", v.vram_total ? v.vram_used / v.vram_total * 100 : null, v.vram_total && `${v.vram_used.toFixed(1)}/${Math.round(v.vram_total)}GB`),
      it("メモリ", v.mem, v.mem != null && `${Math.round(v.mem)}% (${v.mem_used.toFixed(1)}/${Math.round(v.mem_total)}GB)`)].filter(Boolean));
  } catch (e) {}
  setTimeout(sysTick, 2000);
}
sysTick();
