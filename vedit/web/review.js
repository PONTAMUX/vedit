"use strict";
/* 修正点の一覧と修正前との比較(右に付くパネル / 別窓 review.html の両方で使う)。
   修正点の時刻は先方が見た動画(修正前の書き出し)の時刻。サーバー(review.py)が修正前のカットで元素材の時刻 src に直して返すので、
   カットを足して後ろがずれても、クリックで今の正しい所へ飛べる。修正前の動画は再生ヘッドに合わせて動かす(元素材の時刻で対応づけ)。
   Review.mount(root, host) — host: pos() → {src, gap:{at,off}|null, playing, rate} / seek(src, gapAt) / docked / popout() / close() */
window.Review = (() => {
  const el = (tag, attrs = {}, ...kids) => {
    const e = document.createElement(tag);
    for (const [k, v] of Object.entries(attrs)) {
      if (k === "class") e.className = v;
      else if (k.startsWith("on")) e.addEventListener(k.slice(2), v);
      else if (v !== false && v != null) e.setAttribute(k, v === true ? "" : v);
    }
    for (const c of kids.flat()) if (c != null) e.append(c.nodeType ? c : document.createTextNode(c));
    return e;
  };
  const call = async (path, body) => {
    const r = await fetch(path, { method: body === undefined ? "GET" : "POST", headers: { "X-Token": TOKEN, "Content-Type": "application/json" }, body: body === undefined ? undefined : JSON.stringify(body) });
    const j = await r.json(); if (!r.ok || j.error) throw new Error(j.error || r.status); return j;
  };
  const fmt = t => { if (t == null || !isFinite(t)) return "-"; t = Math.round(Math.max(0, t) * 100) / 100; const m = Math.floor(t / 60), s = t - m * 60; return `${m}:${s.toFixed(2).padStart(5, "0")}`; };
  const fmtS = t => { t = Math.max(0, Math.round(t)); return `${Math.floor(t / 60)}:${String(t % 60).padStart(2, "0")}`; };
  const chan = "BroadcastChannel" in window ? new BroadcastChannel("vedit-review-" + TOKEN) : null;

  // 出力上の並び P(review.py pieces と同じ形)で 元素材の秒 → 出力上の秒。空白の中なら gap = {at, off}。並びに無い(カットされた)所は次の素材の頭で missing
  function outOf(P, src, gap) {
    if (gap) { const g = P.find(p => p.gap != null && Math.abs(p.at - gap.at) < 1e-3); if (g) return { o: g.o + Math.min(gap.off, g.gap) }; src = gap.at; }
    for (const p of P) if (p.gap == null && src >= p.s - 1e-6 && src < p.e) return { o: p.o + src - p.s };
    const n = P.find(p => p.gap == null && p.s > src);
    if (n) return { o: n.o, missing: true };
    const L = P[P.length - 1]; return { o: L ? L.o + (L.gap ?? L.e - L.s) : 0, missing: true };
  }

  let root = null, host = null, D = null, vid = null, loading = null, timer = null, lastCur = -1, pvWait = null;
  let R = null; // 見ている回(0 始まり。null = 最新 = 今回)。見出しのプルダウンで選ぶ
  const rq = () => R == null ? "" : `?r=${R}`;

  async function refresh() {
    if (!root) return;
    try { D = await call("/api/review" + rq()); } catch (e) { D = { error: e.message }; }
    render();
  }
  function changed() { if (chan) chan.postMessage({ type: "saved" }); refresh(); }
  async function op(body) { try { D = await call("/api/review", { r: R, ...body }); if (body.op === "start" || body.op === "next") R = null; render(); if (chan) chan.postMessage({ type: "saved" }); } catch (e) { alert("修正点: " + e.message); } }

  function render() {
    if (!root) return;
    const keepScroll = root.querySelector(".rv-body")?.scrollTop || 0;
    // 見出し: 回が2つ以上あればクリックで「修正点1(前回)/ 修正点2(今回)」を選ぶ
    const RS = D && D.rounds || [], last = RS.length - 1;
    const title = RS.length > 1 ? el("select", { class: "rv-round", title: "どの回の修正点を見るか(前回の修正 / 今回の修正)", onchange: e => { R = +e.target.value === last ? null : +e.target.value; lastCur = -1; refresh(); } },
      RS.map(x => { const o = el("option", { value: x.i }, `修正点${x.i + 1}${x.i === last ? "(今回)" : x.i === last - 1 ? "(前回)" : ""}  ${x.n}件`); o.selected = x.i === D.round; return o; }))
      : el("b", {}, "修正点");
    const head = el("div", { class: "rv-head" }, title, el("span", { class: "sp" }),
      host.docked ? el("button", { title: "別の窓に出す(大きくして修正前の映像を見られる)", onclick: () => host.popout() }, "⧉ 別窓")
        : el("button", { title: "編集画面の右に戻す", onclick: () => { if (chan) chan.postMessage({ type: "dock" }); window.close(); } }, "⇤ 右に戻す"),
      host.docked ? el("button", { title: "閉じる", onclick: () => host.close() }, "×") : null);
    const body = el("div", { class: "rv-body" });
    root.replaceChildren(head, body);
    if (!D) { body.append(el("p", { class: "note" }, "読み込み中…")); return; }
    if (D.error) { body.append(el("p", { class: "note warn" }, "読めません: " + D.error)); return; }
    if (!D.exists) {
      body.append(el("p", { class: "note" }, "修正前の控えがまだありません。先方に見せた状態のうちに控えておくと、修正点の時刻(先方が見た動画の時刻)で正しい所へ飛べ、修正前の映像と並べて見られます。"),
        el("p", { class: "note" }, "修正前の動画: " + (D.video_guess || "(見つかりません。後で指定できます)")),
        el("button", { class: "primary", onclick: () => op({ op: "start" }) }, "今の状態を修正前として控える"));
      vid = null; return;
    }
    // 修正前の映像
    if (D.video) {
      // 軽い版(720p・キーフレーム0.5秒おき・音入り)を出す。初回はサーバーが裏で作り始めて元の mp4 を返すので、できたら差し替える
      const key = D.video + (D.video_pv ? "|pv" : "");
      if (!vid || vid.dataset.src !== key) {
        const old = vid;
        vid = el("video", { playsinline: true, preload: "auto", muted: true });
        vid.muted = old ? old.muted : true; vid.src = `/media?p=${encodeURIComponent(D.video)}&pv=a&t=${TOKEN}`; vid.dataset.src = key;
        if (old) { old.pause(); old.removeAttribute("src"); old.load(); }
      }
      clearTimeout(pvWait); if (!D.video_pv) pvWait = setTimeout(refresh, 15000);
      const snd = el("input", { type: "checkbox", onchange: e => { vid.muted = !e.target.checked; } }); snd.checked = !vid.muted;
      body.append(el("div", { class: "rv-vid" }, vid, el("div", { class: "rv-cap" })),
        el("label", { class: "chk rv-snd", title: "修正前の動画の音を出す(本編の音は 🔊 で消せます)" }, snd, " 修正前の音を出す"));
    } else {
      vid = null;
      body.append(el("p", { class: "note" }, "修正前の動画が指定されていません(書き出した mp4 を project と同じフォルダに置いて「控え直す」か、vedit review … start --video)。"));
    }
    // 修正点
    const P = D.points, done = P.filter(p => p.done).length;
    body.append(el("h4", {}, `${RS.length > 1 ? `修正点${D.round + 1}` : "修正点"} ${P.length}件(済 ${done})`));
    const ul = el("ul", { class: "rv-list" });
    for (const p of P) {
      const now = outOf(D.cur.pieces, p.src, null);
      const cb = el("input", { type: "checkbox", title: "済みにする", onclick: e => e.stopPropagation(), onchange: e => op({ op: "update", i: p.i, done: e.target.checked }) }); cb.checked = !!p.done;
      ul.append(el("li", { class: "rv-pt" + (p.done ? " done" : ""), "data-t": p.t, title: `クリックでその所へ(元素材 ${fmt(p.src)})`, onclick: () => host.seek(p.src, p.gap) },
        cb, el("span", { class: "rv-t", title: "今の動画での時刻(カットで前が縮んだ分を直した時刻)" }, fmtS(now.o)),
        el("span", { class: "rv-now", title: "先方が見た動画(修正前)での時刻" }, now.missing ? "カット済" : fmtS(now.o) === fmtS(p.t) ? "" : `(先方 ${fmtS(p.t)})`),
        el("span", { class: "rv-tx" }, p.text),
        el("button", { class: "rv-del", title: "この修正点を消す", onclick: e => { e.stopPropagation(); if (confirm(`「${fmtS(p.t)} ${p.text}」を消しますか?`)) op({ op: "delete", i: p.i }); } }, "×")));
    }
    body.append(ul);
    if (P.length) body.append(el("div", { class: "row" }, el("button", { title: "vreview の管理ページに貼る文章(1行1点・時刻は今の動画。カットで消えた点は継ぎ目に[カット済])をクリップボードへ", onclick: async e => {
      const b = e.currentTarget;
      try {
        const r = await fetch("/api/review/export" + rq(), { headers: { "X-Token": TOKEN } }), tx = await r.text();
        if (r.status === 404) throw new Error("この編集画面はこの機能を足す前に開いたものです。vedit を開き直してください");
        if (!r.ok) throw new Error((() => { try { return JSON.parse(tx).error; } catch { return r.status; } })());
        await navigator.clipboard.writeText(tx); b.textContent = `コピーしました(${tx.trim().split("\n").length}件)`; setTimeout(() => { b.textContent = "📋 vreview 用にコピー"; }, 2000);
      } catch (err) { alert("修正点の出力: " + err.message); }
    } }, "📋 vreview 用にコピー")));
    const ta = el("textarea", { rows: 3, placeholder: "0:07 画像\n3:24 2回言ってるから1回目カット" });
    body.append(el("details", { class: "rv-add" }, el("summary", {}, "修正点を足す(先方の文をそのまま貼れます)"), ta,
      el("div", { class: "row" }, el("button", { onclick: () => ta.value.trim() && op({ op: "add", text: ta.value }) }, "足す"))));
    // 修正前から変えた所
    const C = D.changes || [];
    body.append(el("h4", {}, `修正前から変えた所 ${C.length}件`));
    const cl = el("ul", { class: "rv-list rv-chg" });
    for (const c of C) {
      const now = c.src == null ? null : outOf(D.cur.pieces, c.src, null);
      cl.append(el("li", { class: "rv-pt", title: c.src == null ? "" : "クリックでその所へ", onclick: () => c.src != null && host.seek(c.src, null) },
        el("span", { class: "rv-t" }, now ? fmtS(now.o) : ""), el("span", { class: "rv-what" }, c.what), el("span", { class: "rv-tx" }, c.label)));
    }
    if (!C.length) cl.append(el("li", { class: "note" }, "まだありません"));
    body.append(cl, el("p", { class: "note" }, `控え: ${D.created || ""}  修正前 ${fmt(D.base.total)} → 今 ${fmt(D.cur.total)}`),
      el("div", { class: "row" }, el("button", { title: "今の状態(先方に見せ直す版)を修正前として控え、次の回の修正点を始める(前の回の修正点は残り、上のプルダウンで見られます)", onclick: () => confirm(`今の状態を修正前として控え、修正点${RS.length + 1} を始めます。前の回の修正点は残ります。よいですか?`) && op({ op: "next" }) }, "次の回を始める"),
        el("button", { title: "今の状態で修正前を作り直す(すべての回の修正点が消えます)", onclick: () => confirm("今の状態を修正前として控え直します。すべての回の修正点が消えます。よいですか?") && op({ op: "start", force: true }) }, "控え直す")));
    body.scrollTop = keepScroll; lastCur = -1;
  }

  // 再生ヘッドに合わせて修正前の映像を動かす・今の修正点に印
  function tick() {
    if (!root || !D || !D.exists) return;
    const pos = host.pos();
    const cap = root.querySelector(".rv-cap");
    if (!pos) { if (cap) cap.textContent = "編集画面からの位置を待っています…"; if (vid && !vid.paused) vid.pause(); return; }
    const b = outOf(D.base.pieces, pos.src, pos.gap), c = outOf(D.cur.pieces, pos.src, pos.gap);
    if (cap) cap.textContent = `修正前 ${fmt(b.o)}${b.missing ? "(修正前に無い所)" : ""}  /  今 ${fmt(c.o)}`;
    let cur = -1; D.points.forEach((p, i) => { if (p.t <= b.o + 0.5) cur = i; });
    if (cur !== lastCur) { lastCur = cur; root.querySelectorAll(".rv-list:not(.rv-chg) .rv-pt").forEach((li, i) => li.classList.toggle("cur", i === cur)); }
    if (!vid || vid.readyState < 1) return;
    const want = Math.min(b.o, (vid.duration || 1e9) - 0.05);
    if (pos.playing && pos.rate > 0) {
      // 再生中はシークしない(シークのたびにデコードし直して重く、本編の再生まで引っかかる)。小さいずれは速さを少し変えて追いつき、
      // 大きく飛んだ時(修正で変わった所・本編のシーク)だけシークする
      const d = want - vid.currentTime;
      if (Math.abs(d) > 1.5 && !vid.seeking) { vid.currentTime = want; vid.playbackRate = pos.rate; }
      else { const r = pos.rate * (1 + Math.max(-0.1, Math.min(0.1, d * 0.5))); if (Math.abs(vid.playbackRate - r) > 0.01) vid.playbackRate = r; }
      if (vid.paused) vid.play().catch(() => {});
    } else {
      if (!vid.paused) vid.pause();
      if (!vid.seeking && Math.abs(vid.currentTime - want) > 0.03) vid.currentTime = want;
    }
  }

  function mount(r, h) {
    root = r; host = h; root.classList.add("rv");
    if (!timer) timer = setInterval(tick, 80);
    refresh();
  }
  function unmount() { if (vid) vid.pause(); vid = null; if (root) root.replaceChildren(); root = null; clearInterval(timer); timer = null; clearTimeout(pvWait); }
  return { mount, unmount, refresh, changed, chan, outOf };
})();
