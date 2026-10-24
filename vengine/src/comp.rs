//! 1区間を作る: 背景のグラフ + 差し込み(出番の少し前に読み始める)を重ねて、エンコーダーへ。
//! 重ね方は ffmpeg の overlay(format=yuv420)と同じ(engine.py と同じ計算): 位置は偶数に切り捨て、
//! 色差の透明度は 2x2 の平均、(d*(255-a)+s*a)/255 は ffmpeg の FAST_DIV255。
use crate::av::*;
use crate::enc::{Enc, EncDef};
use crate::graph::{GraphDef, Runner};
use ffmpeg_sys_next as ff;
use serde::Deserialize;

const PREFETCH: f64 = 2.0; // 差し込みを出番の何秒前から読み始めるか
const POOL: usize = 4; // 背景・差し込みごとに先読みしておくコマ数

#[allow(non_snake_case)]
#[derive(Deserialize)]
pub struct Job {
    #[serde(default)]
    pub cwd: String,
    pub W: i32,
    pub H: i32,
    pub F: f64,
    pub rate: String,
    pub N: i64,
    pub m: i64,
    pub base: Vec<String>,
    pub enc: Vec<String>,
    pub layers: Vec<LayerDef>,
}

#[derive(Deserialize, Clone)]
pub struct LayerDef {
    pub file: String,
    pub img: bool,
    pub w: i32,
    pub h: i32,
    pub x: f64,
    pub y: f64,
    pub a: f64,
    pub dur: f64,
    pub bd: f64,
    #[serde(rename = "in")]
    pub in_: String,
    pub out: String,
    pub m0: i64,
    pub m1: i64,
    #[serde(default = "yes")]
    pub alpha: bool,
    pub dec: Vec<String>,
}
fn yes() -> bool {
    true
}

/// 平面1枚(先頭, 1行のバイト数, 幅, 高さ)
#[derive(Clone, Copy)]
struct Pl {
    p: *mut u8,
    ls: isize,
    w: i32,
    h: i32,
}

impl Pl {
    fn of(f: &Frame, i: usize, w: i32, h: i32) -> Pl {
        let (p, ls) = f.plane(i);
        Pl { p, ls, w, h }
    }
    /// (x, y) から w×h を切り出したもの
    fn sub(self, x: i32, y: i32, w: i32, h: i32) -> Pl {
        Pl { p: unsafe { self.p.offset(y as isize * self.ls + x as isize) }, ls: self.ls, w, h }
    }
    fn row<'a>(&self, y: i32) -> &'a mut [u8] {
        unsafe { std::slice::from_raw_parts_mut(self.p.offset(y as isize * self.ls), self.w as usize) }
    }
}

/// 透明度の平面(自前のメモリ)
#[derive(Default)]
struct Alpha {
    buf: Vec<u8>,
    w: i32,
    h: i32,
}
impl Alpha {
    fn pl(&mut self) -> Pl {
        Pl { p: self.buf.as_mut_ptr(), ls: self.w as isize, w: self.w, h: self.h }
    }
}

/// dst の (x,y) と src が重なる範囲 → (dst の x0, y0, src の x0, y0, 幅, 高さ)
fn clip(d: &Pl, s: &Pl, x: i32, y: i32) -> Option<(i32, i32, i32, i32, i32, i32)> {
    let (x0, y0, x1, y1) = (x.max(0), y.max(0), d.w.min(x + s.w), d.h.min(y + s.h));
    if x1 <= x0 || y1 <= y0 { None } else { Some((x0, y0, x0 - x, y0 - y, x1 - x0, y1 - y0)) }
}

/// そのまま写す(透明のないもの)
fn put(d: Pl, s: Pl, x: i32, y: i32) {
    if let Some((dx, dy, sx, sy, w, h)) = clip(&d, &s, x, y) {
        for j in 0..h {
            let dr = &mut d.row(dy + j)[dx as usize..(dx + w) as usize];
            dr.copy_from_slice(&s.row(sy + j)[sx as usize..(sx + w) as usize]);
        }
    }
}

/// 透明度 a で重ねる
fn blend(d: Pl, s: Pl, a: Pl, x: i32, y: i32) {
    if let Some((dx, dy, sx, sy, w, h)) = clip(&d, &s, x, y) {
        let (w, sx, dx) = (w as usize, sx as usize, dx as usize);
        for j in 0..h {
            let dr = &mut d.row(dy + j)[dx..dx + w];
            let sr = &s.row(sy + j)[sx..sx + w];
            let ar = &a.row(sy + j)[sx..sx + w];
            for ((dv, &sv), &av) in dr.iter_mut().zip(sr).zip(ar) {
                let (dd, ss, aa) = (*dv as u32, sv as u32, av as u32);
                let v = ss * aa + dd * (255 - aa);
                *dv = (((v + 128) * 257) >> 16) as u8;
            }
        }
    }
}

fn clip01(x: f64) -> f64 {
    x.clamp(0.0, 1.0)
}

/// 位置(build の overlay の x/y の式と同じ計算)
fn pos_at(l: &LayerDef, t: f64, w: i32, h: i32) -> (i32, i32) {
    let (mut x, mut y, lw, lh, dur) = (l.x, l.y, l.w as f64, l.h as f64, l.dur);
    let p_in = if dur > 0.0 { 1.0 - clip01((t - l.a) / dur) } else { 0.0 };
    let p_out = if dur > 0.0 { clip01((t - l.bd) / dur) } else { 0.0 };
    for (kind, p) in [(&l.in_, p_in), (&l.out, p_out)] {
        match kind.as_str() {
            "slide-left" => x -= p * (x + lw),
            "slide-right" => x += p * (w as f64 - x),
            "slide-up" => y += p * (h as f64 - y),
            "slide-down" => y -= p * (y + lh),
            _ => {}
        }
    }
    ((x as i64 & !1) as i32, (y as i64 & !1) as i32) // overlay(yuv420)は小数を切り捨てて偶数にそろえる
}

/// fade=alpha=1 の掛け率(0〜65535)。None は掛けない
fn fade_at(l: &LayerDef, t: f64) -> Option<i64> {
    let mut f = None;
    let d = l.dur;
    if l.in_ == "fade" {
        f = Some(if d <= 0.0 || t >= l.a + d { 65535 } else { 0.max(((t - l.a) * 65535.0 / d) as i64) });
    }
    if l.out == "fade" {
        let g = if d <= 0.0 || t >= l.bd + d { 0 } else { 65535.min(65535 - 0.max(((t - l.bd) * 65535.0 / d) as i64)) };
        f = Some(match f {
            None => g,
            Some(f) => (f * g) / 65535,
        });
    }
    f
}

struct Layer {
    l: LayerDef,
    run: Option<Runner>,
    cur: Option<Frame>,
    have: bool,
    done: bool,
    solid: bool,
    // 画像は透明でない四角(偶数にそろえる)だけを重ねる: 絵の中の位置と大きさ
    ox: i32,
    oy: i32,
    cw: i32,
    ch: i32,
    empty: bool,
    a: Alpha,
    ac: Alpha,
    fixed: bool, // a / ac が画像の(フェードなしの)ものを作り置きしたもの
}

impl Layer {
    fn new(l: LayerDef) -> Layer {
        let solid = !l.alpha;
        let (cw, ch) = (l.w, l.h);
        Layer { l, run: None, cur: None, have: false, done: false, solid, ox: 0, oy: 0, cw, ch, empty: false,
                a: Alpha::default(), ac: Alpha::default(), fixed: false }
    }

    fn start(&mut self, cwd: &str) -> R<()> {
        if self.run.is_none() && !self.done {
            let mut def = GraphDef::from_args(&self.l.dec, cwd)?;
            def.pix_fmt = Some(if self.l.alpha { "yuva420p" } else { "yuv420p" }.into());
            def.threads = 2;
            if self.l.img {
                def.frames = Some(1);
            }
            self.run = Some(Runner::spawn(def, cwd.to_string(), true, POOL));
        }
        Ok(())
    }

    fn close(&mut self) {
        self.run = None;
        self.done = true;
        self.cur = None;
        self.a = Alpha::default();
        self.ac = Alpha::default();
    }

    /// 次の絵を受け取る。読めなければ false(動画が終わった = それ以降は出さない)
    fn fetch(&mut self, cwd: &str) -> R<bool> {
        if self.done {
            return Ok(false);
        }
        if self.l.img && self.have {
            return Ok(true);
        }
        self.start(cwd)?;
        let f = self.run.as_mut().unwrap().recv().map_err(|e| format!("差し込みを読めませんでした {}: {e}", self.l.file))?;
        let Some(f) = f else {
            if !self.have {
                return Err(format!("差し込みを読めませんでした {}: 絵がありません", self.l.file));
            }
            self.close();
            return Ok(false);
        };
        let (fw, fh) = (f.f().width, f.f().height);
        if (fw, fh) != (self.l.w, self.l.h) {
            return Err(format!("差し込みの大きさが違います {}: {fw}x{fh}", self.l.file));
        }
        self.cur = Some(f);
        self.have = true;
        if self.l.img {
            // 画像は透明なところがあるか調べ、透明でない四角だけにする
            let f = self.cur.as_ref().unwrap();
            let a = Pl::of(f, 3, fw, fh);
            let (mut x0, mut y0, mut x1, mut y1) = (fw, fh, -1, -1);
            let mut solid = true;
            for j in 0..fh {
                let r = a.row(j);
                if r.iter().any(|&v| v != 255) {
                    solid = false;
                }
                if let Some(i) = r.iter().position(|&v| v != 0) {
                    let k = r.iter().rposition(|&v| v != 0).unwrap() as i32;
                    x0 = x0.min(i as i32);
                    x1 = x1.max(k);
                    y0 = y0.min(j);
                    y1 = y1.max(j);
                }
            }
            self.solid = solid;
            if !solid {
                if y1 < 0 {
                    self.empty = true;
                } else {
                    let (x0, y0) = (x0 & !1, y0 & !1);
                    let (x1, y1) = (fw.min((x1 + 2) & !1), fh.min((y1 + 2) & !1));
                    (self.ox, self.oy, self.cw, self.ch) = (x0, y0, x1 - x0, y1 - y0);
                }
            }
            self.run = None; // 1枚読めば終わり
        }
        Ok(true)
    }

    /// 今の絵の平面 (Y, U, V)(画像は透明でない四角だけ)
    fn planes(&self) -> (Pl, Pl, Pl) {
        let f = self.cur.as_ref().unwrap();
        let (w, h) = (self.l.w, self.l.h);
        let (cx, cy, cw, ch) = (self.ox, self.oy, self.cw, self.ch);
        (Pl::of(f, 0, w, h).sub(cx, cy, cw, ch),
         Pl::of(f, 1, w / 2, h / 2).sub(cx / 2, cy / 2, cw / 2, ch / 2),
         Pl::of(f, 2, w / 2, h / 2).sub(cx / 2, cy / 2, cw / 2, ch / 2))
    }

    /// 重ねる透明度 (輝度用, 色差用)。f はフェードの掛け率
    fn prep(&mut self, f: Option<i64>) -> (Pl, Pl) {
        if !(self.fixed && f.is_none()) {
            let (cw, ch) = (self.cw, self.ch);
            let src = if self.l.alpha {
                let fr = self.cur.as_ref().unwrap();
                Some(Pl::of(fr, 3, self.l.w, self.l.h).sub(self.ox, self.oy, cw, ch))
            } else {
                None
            };
            let a = &mut self.a;
            a.w = cw;
            a.h = ch;
            a.buf.resize((cw * ch) as usize, 0);
            for j in 0..ch {
                let dr = &mut a.buf[(j * cw) as usize..((j + 1) * cw) as usize];
                match (&src, f) {
                    (Some(s), None) => dr.copy_from_slice(s.row(j)),
                    (Some(s), Some(f)) => {
                        for (d, &v) in dr.iter_mut().zip(s.row(j).iter()) {
                            *d = ((v as i64 * f + 32768) >> 16) as u8;
                        }
                    }
                    (None, None) => dr.fill(255),
                    (None, Some(f)) => dr.fill(((255 * f + 32768) >> 16) as u8),
                }
            }
            let (hw, hh) = (cw / 2, ch / 2);
            let ac = &mut self.ac;
            ac.w = hw;
            ac.h = hh;
            ac.buf.resize((hw * hh) as usize, 0);
            for j in 0..hh as usize {
                let r0 = &a.buf[2 * j * cw as usize..];
                let r1 = &a.buf[(2 * j + 1) * cw as usize..];
                let dr = &mut ac.buf[j * hw as usize..(j + 1) * hw as usize];
                for (i, d) in dr.iter_mut().enumerate() {
                    let s = r0[2 * i] as u32 + r0[2 * i + 1] as u32 + r1[2 * i] as u32 + r1[2 * i + 1] as u32;
                    *d = (s >> 2) as u8;
                }
            }
            self.fixed = self.l.img && f.is_none();
        }
        (self.a.pl(), self.ac.pl())
    }
}

/// 1区間を作る。progress(n) は作ったコマ数
pub fn run(job: &Job, out: &str, progress: &dyn Fn(i64)) -> R<()> {
    let (w, h, fps, n_all, m_start) = (job.W, job.H, job.F, job.N, job.m);
    let cwd = job.cwd.as_str();
    let mut bdef = GraphDef::from_args(&job.base, cwd)?;
    bdef.pix_fmt = Some("yuv420p".into());
    let mut base = Runner::spawn(bdef, cwd.to_string(), true, POOL);
    let mut enc = Enc::spawn(EncDef::from_args(&job.enc)?, w, h, parse_rate(&job.rate)?, cwd.to_string(), out.to_string(), POOL);
    let mut layers: Vec<Layer> = job.layers.iter().cloned().map(Layer::new).collect();
    let pre = (PREFETCH * fps).round() as i64;
    // VENGINE_PROF=1 で、どこで待ったかを出す(差し込み, 背景, 重ね合わせ, エンコーダー への受け渡し)
    let prof = std::env::var_os("VENGINE_PROF").is_some();
    let mut tm = [0f64; 4];
    let mut clk = std::time::Instant::now();
    let mut lap = |i: usize, tm: &mut [f64; 4]| {
        if prof {
            let now = std::time::Instant::now();
            tm[i] += (now - clk).as_secs_f64();
            clk = now;
        }
    };
    for n in 0..n_all {
        let m = m_start + n;
        let t = m as f64 / fps;
        let mut act = vec![];
        for (i, ly) in layers.iter_mut().enumerate() {
            if ly.done {
                continue;
            }
            if m > ly.l.m1 {
                ly.close();
                continue;
            }
            if m >= ly.l.m0 - pre {
                ly.start(cwd)?; // 先に読み始めておく
            }
            if ly.l.m0 <= m && ly.fetch(cwd)? && !ly.empty {
                act.push(i);
            }
        }
        lap(0, &mut tm);
        let b = base.recv()?.ok_or_else(|| format!("背景が {n} コマで終わりました"))?;
        lap(1, &mut tm);
        if (b.f().width, b.f().height) != (w, h) {
            return Err(format!("背景の大きさが違います: {}x{}", b.f().width, b.f().height));
        }
        let frame = if act.is_empty() {
            b
        } else {
            // 位置・フェード。上から見て、画面を丸ごと隠す不透明なもの(s0)より下は描かない
            let mut st = vec![];
            for &i in &act {
                let ly = &layers[i];
                let (x, y) = pos_at(&ly.l, t, w, h);
                let f = fade_at(&ly.l, t);
                st.push((i, x + ly.ox, y + ly.oy, f, ly.solid && f.is_none()));
            }
            let s0 = st.iter().rposition(|&(i, x, y, _, op)| {
                let l = &layers[i].l;
                op && x <= 0 && y <= 0 && x + l.w >= w && y + l.h >= h
            });
            let top = st.len() - 1;
            if s0 == Some(top) && !layers[st[top].0].l.img && (st[top].1, st[top].2, layers[st[top].0].l.w, layers[st[top].0].l.h) == (0, 0, w, h) {
                layers[st[top].0].cur.as_ref().unwrap().share() // いちばん上が全画面の不透明な動画なら、その絵をそのまま渡す
            } else {
                let mut o = if s0.is_some() {
                    // 背景は丸ごと隠れる: 新しい絵に描く
                    let o = Frame::new();
                    unsafe {
                        (*o.0).format = ff::AVPixelFormat::AV_PIX_FMT_YUV420P as i32;
                        (*o.0).width = w;
                        (*o.0).height = h;
                        check(ff::av_frame_get_buffer(o.0, 0), "絵のメモリ")?;
                    }
                    o
                } else {
                    let b = b;
                    check(unsafe { ff::av_frame_make_writable(b.0) }, "絵のメモリ")?;
                    b
                };
                let (dy, du, dv) = (Pl::of(&o, 0, w, h), Pl::of(&o, 1, w / 2, h / 2), Pl::of(&o, 2, w / 2, h / 2));
                for &(i, x, y, f, op) in &st[s0.unwrap_or(0)..] {
                    let ly = &mut layers[i];
                    let (sy, su, sv) = ly.planes();
                    if op {
                        put(dy, sy, x, y);
                        put(du, su, x / 2, y / 2);
                        put(dv, sv, x / 2, y / 2);
                    } else {
                        let (a, ac) = ly.prep(f);
                        blend(dy, sy, a, x, y);
                        blend(du, su, ac, x / 2, y / 2);
                        blend(dv, sv, ac, x / 2, y / 2);
                    }
                }
                let _ = o.fm();
                o
            }
        };
        lap(2, &mut tm);
        let sent = enc.tx.as_ref().unwrap().send(frame);
        lap(3, &mut tm);
        if sent.is_err() {
            break; // エンコーダーが止まった(理由は finish で)
        }
        if n % 15 == 0 {
            progress(n);
        }
    }
    drop(layers);
    drop(base);
    let got = enc.finish()?;
    if prof {
        eprintln!("prof {out}: layers {:.1}s base {:.1}s comp {:.1}s enc {:.1}s", tm[0], tm[1], tm[2], tm[3]);
    }
    if got != n_all {
        return Err(format!("{got} コマしか書けませんでした({n_all} コマのはず)"));
    }
    progress(n_all);
    Ok(())
}
