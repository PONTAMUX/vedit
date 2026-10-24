"use strict";
/* Premiere 風オーディオメーター(タイムライン右端の縦型 L/R)。
   app.js の setupMeter / updateMeter をこのファイルの function 宣言で置き換える(app.js の後に読み込むこと)。
   - 各 media element → bus(2ch, speakers で mono→L/R 複製) → destination(音はここで必ず出る)
                                                     └→ ChannelSplitter → Analyser L / R
   - 表示は dBFS。プレビュー音量 masterVol() の分は差し引く(書き出し相当の値)。
   - window.meterConnect(el): 後から media element をメーターにつなぐ。setupMeter 前に呼ばれたら保留し、setupMeter 時に接続する。 */
const VM = {
  ctx: null, bus: null, anl: [], buf: null,
  pending: new Set(), done: new WeakSet(),
  MIN: -60,                       // 目盛りの下端(= -∞ の位置)
  FALL: 22, HOLD_MS: 1500, HOLD_FALL: 12, RMS_TAU: 0.3,
  lv: [-Infinity, -Infinity],     // 表示中のピーク(落下つき)
  hold: [-Infinity, -Infinity], holdAt: [0, 0],
  pw: [0, 0],                     // RMS 用の平均パワー(リニア)
  clip: [false, false],
  raf: 0, last: 0, cv: null, g: null, w: 0, h: 0, grad: null,
};

window.meterConnect = function (el) {
  if (!el || VM.done.has(el)) return;
  if (!VM.ctx) { VM.pending.add(el); return; }
  VM.pending.delete(el); VM.done.add(el);   // createMediaElementSource は1要素1回だけ
  try { VM.ctx.createMediaElementSource(el).connect(VM.bus); }
  catch (e) { console.warn("meterConnect:", e); }
};

function setupMeter() {
  if (!VM.ctx) {
    try {
      const ctx = new AudioContext();
      const bus = ctx.createGain();
      bus.channelCount = 2; bus.channelCountMode = "explicit"; bus.channelInterpretation = "speakers";
      bus.connect(ctx.destination);
      const sp = ctx.createChannelSplitter(2); bus.connect(sp);
      VM.anl = [0, 1].map(i => { const a = ctx.createAnalyser(); a.fftSize = 2048; a.smoothingTimeConstant = 0; sp.connect(a, i); return a; });
      VM.buf = new Float32Array(2048);
      VM.ctx = ctx; VM.bus = bus;
    } catch (e) { VM.ctx = null; console.warn("setupMeter:", e); return; }
    // app.js の再生用 media element(aud2 = 音声だけずらした区間の再生用。無い版の app.js でも動くよう typeof で確認)
    for (const el of [typeof vid !== "undefined" && vid, typeof aud !== "undefined" && aud, typeof bgm !== "undefined" && bgm, typeof aud2 !== "undefined" && aud2]) if (el) VM.pending.add(el);
  }
  if (VM.ctx.state === "suspended") VM.ctx.resume();
  for (const el of [...VM.pending]) window.meterConnect(el);
  meterKick();
}
function updateMeter() { meterKick(); }   // 描画は meter.js 自身の rAF で行う(停止中の減衰も含む)

function meterReset() {
  VM.hold = [-Infinity, -Infinity]; VM.clip = [false, false];
  if (typeof peakHold !== "undefined") peakHold = -Infinity;
  meterKick(true);
}
function meterKick(force) {
  if (!VM.raf) { VM.last = performance.now(); VM.raf = requestAnimationFrame(meterFrame); }
  if (force) meterDraw();
}

const vmDb = v => v > 1e-6 ? 20 * Math.log10(v) : -Infinity;
const vmFmt = d => isFinite(d) && d > VM.MIN ? d.toFixed(1) : "-∞";
function meterFrame(now) {
  VM.raf = 0;
  const dt = Math.min(0.1, Math.max(0, (now - VM.last) / 1000)); VM.last = now;
  const isPlaying = typeof playing !== "undefined" && playing;
  const mv = typeof masterVol === "function" ? masterVol() : 1;
  const comp = mv > 0 ? 20 * Math.log10(mv) : 0;
  let active = false;
  for (let c = 0; c < 2; c++) {
    let pk = -Infinity, p = 0;
    if (VM.ctx && isPlaying && mv > 0 && VM.anl[c]) {
      const b = VM.buf; VM.anl[c].getFloatTimeDomainData(b);
      let m = 0, sq = 0; for (let i = 0; i < b.length; i++) { const x = b[i], a = x < 0 ? -x : x; if (a > m) m = a; sq += x * x; }
      pk = vmDb(m) - comp; p = sq / b.length / (mv * mv);
    }
    if (pk > 0) VM.clip[c] = true;
    // 落下(ballistics): 上がるのは即時、下がるのは FALL dB/s
    const fallen = isFinite(VM.lv[c]) ? VM.lv[c] - VM.FALL * dt : -Infinity;
    VM.lv[c] = Math.max(pk, fallen); if (VM.lv[c] < VM.MIN) VM.lv[c] = -Infinity;
    // ピークホールド: HOLD_MS 保持してから HOLD_FALL dB/s で下がる
    if (pk >= VM.hold[c]) { VM.hold[c] = pk; VM.holdAt[c] = now; }
    else if (now - VM.holdAt[c] > VM.HOLD_MS) { VM.hold[c] = Math.max(VM.lv[c], VM.hold[c] - VM.HOLD_FALL * dt); if (VM.hold[c] < VM.MIN) VM.hold[c] = -Infinity; }
    const k = 1 - Math.exp(-dt / VM.RMS_TAU); VM.pw[c] += (p - VM.pw[c]) * k; if (VM.pw[c] < 1e-12) VM.pw[c] = 0;
    if (isFinite(VM.lv[c]) || isFinite(VM.hold[c]) || VM.pw[c] > 0) active = true;
  }
  meterDraw();
  if (isPlaying || active) VM.raf = requestAnimationFrame(meterFrame);
}

function meterLayout() {
  const cv = VM.cv; if (!cv) return;
  const r = cv.getBoundingClientRect(), dpr = window.devicePixelRatio || 1;
  VM.w = Math.max(1, Math.round(r.width)); VM.h = Math.max(1, Math.round(r.height));
  cv.width = Math.round(VM.w * dpr); cv.height = Math.round(VM.h * dpr);
  VM.g = cv.getContext("2d"); VM.g.setTransform(dpr, 0, 0, dpr, 0, 0);
  VM.grad = null; meterDraw();
}
const VM_TOP = 4, VM_BOT = 14, VM_BW = 15, VM_GAP = 2;
const vmY = d => { const t = VM_TOP, b = VM.h - VM_BOT; if (!isFinite(d) || d <= VM.MIN) return b; return t + Math.max(0, Math.min(-d, -VM.MIN)) / -VM.MIN * (b - t); };
const vmColor = d => d >= -0.5 ? "#ff3030" : d >= -6 ? "#f2d43a" : d >= -18 ? "#7be05c" : "#2fbf55";
function meterDraw() {
  const g = VM.g; if (!g) return;
  const W = VM.w, H = VM.h, top = VM_TOP, bot = H - VM_BOT;
  g.clearRect(0, 0, W, H);
  if (!VM.grad) {
    const gr = g.createLinearGradient(0, bot, 0, top), at = d => (d - VM.MIN) / -VM.MIN;
    gr.addColorStop(0, "#127a36"); gr.addColorStop(at(-30), "#25b54c"); gr.addColorStop(at(-12), "#5fe05a");
    gr.addColorStop(at(-6.01), "#a8e04a"); gr.addColorStop(at(-6), "#f2d43a"); gr.addColorStop(at(-1.6), "#f5b830");
    gr.addColorStop(at(-0.6), "#ff8a2a"); gr.addColorStop(at(-0.5), "#ff3030"); gr.addColorStop(1, "#ff3030");
    VM.grad = gr;
  }
  const ticks = [0, -6, -12, -18, -24, -30, -36, -42, -48, -54];
  for (let c = 0; c < 2; c++) {
    const x = c * (VM_BW + VM_GAP);
    g.fillStyle = "#0e0e11"; g.fillRect(x, top, VM_BW, bot - top);
    g.globalAlpha = 0.13; g.fillStyle = VM.grad; g.fillRect(x, top, VM_BW, bot - top); g.globalAlpha = 1;  // 消灯部分(LED のうっすら色)
    const d = VM.lv[c];
    if (isFinite(d)) { const y = vmY(d); g.fillStyle = VM.grad; g.fillRect(x, y, VM_BW, bot - y); }
    const hd = VM.hold[c];
    if (isFinite(hd) && hd > VM.MIN) { const y = Math.round(vmY(hd)); g.fillStyle = vmColor(hd); g.fillRect(x, Math.max(top, y - 1), VM_BW, 2); }
    g.fillStyle = "rgba(0,0,0,.55)"; for (const t of ticks) g.fillRect(x, Math.round(vmY(t)), VM_BW, 1);
  }
  // 目盛り
  const sx = 2 * VM_BW + VM_GAP + 3;
  g.font = "9px Consolas, monospace"; g.textBaseline = "middle"; g.textAlign = "left";
  for (const t of ticks) {
    const y = Math.round(vmY(t)) + 0.5;
    g.fillStyle = t >= -6 ? (t === 0 ? "#e07a7a" : "#cfc27a") : "#8d8d99"; g.fillRect(sx - 2, y - 0.5, 3, 1);
    g.fillText(String(t), sx + 3, y);
  }
  g.fillStyle = "#8d8d99"; g.fillText("-∞", sx + 3, bot - 3);
  g.textAlign = "center"; g.textBaseline = "alphabetic"; g.fillStyle = "#a0a0ab";
  g.fillText("L", VM_BW / 2, H - 2); g.fillText("R", VM_BW * 1.5 + VM_GAP, H - 2);
  g.textAlign = "left"; g.fillStyle = "#6b6b77"; g.fillText("dB", sx + 3, H - 2);
  // ランプと数値
  const cl = VM.clipEls; if (cl) { cl[0].classList.toggle("on", VM.clip[0]); cl[1].classList.toggle("on", VM.clip[1]); }
  const hp = Math.max(VM.hold[0], VM.hold[1]), rp = Math.max(VM.pw[0], VM.pw[1]);
  if (VM.pkEl) { VM.pkEl.textContent = vmFmt(hp); VM.pkEl.classList.toggle("hot", hp > 0); }
  if (VM.rmsEl) VM.rmsEl.textContent = vmFmt(rp > 0 ? 10 * Math.log10(rp) : -Infinity);
}

(function meterInit() {
  const root = document.getElementById("meter"); if (!root) return;
  VM.cv = root.querySelector("canvas");
  VM.clipEls = [root.querySelector("#vm-clip-l"), root.querySelector("#vm-clip-r")];
  VM.pkEl = root.querySelector("#vm-pk"); VM.rmsEl = root.querySelector("#vm-rms");
  root.addEventListener("click", meterReset);
  if (VM.cv) { new ResizeObserver(meterLayout).observe(VM.cv); meterLayout(); }
})();
