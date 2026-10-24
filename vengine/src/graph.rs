//! ffmpeg のフィルタグラフを、ffmpeg の CLI と同じように動かす(-filter_complex / -vf)。
//! 1つのグラフは裏のスレッドで回し、できた絵を先読みして渡す(背景・差し込み・合成・エンコードが同時に進む)。
use crate::av::*;
use crate::input::{Decoder, InSpec, parse_args};
use ffmpeg_sys_next as ff;
use std::ffi::CStr;
use std::path::Path;
use std::ptr;
use std::sync::Mutex;
use std::sync::mpsc::{Receiver, sync_channel};
use std::thread::JoinHandle;

/// グラフを組む間だけ作業フォルダを変える(ass=subs.ass などの相対パスのため)。プロセスで1つなので順番に
pub static CWD: Mutex<()> = Mutex::new(());

/// cwd を作業フォルダにして f を動かす
pub fn with_cwd<T>(cwd: &str, f: impl FnOnce() -> T) -> T {
    let _lock = CWD.lock().unwrap_or_else(|e| e.into_inner());
    let prev = std::env::current_dir().ok();
    if !cwd.is_empty() {
        let _ = std::env::set_current_dir(cwd);
    }
    let r = f();
    if let Some(p) = prev {
        let _ = std::env::set_current_dir(p);
    }
    r
}

pub struct GraphDef {
    pub ins: Vec<InSpec>,
    pub desc: String,
    pub out: Option<String>, // 使う出口のラベル(None = 1つだけのもの)
    pub frames: Option<i64>,
    pub threads: i32,
    pub pix_fmt: Option<String>, // 最後にそろえる形式
}

impl GraphDef {
    /// ffmpeg の引数(入力 + -filter_complex_script / -vf + -map + -frames:v)から
    pub fn from_args(args: &[String], cwd: &str) -> R<GraphDef> {
        let (mut ins, rest) = parse_args(args)?;
        for i in ins.iter_mut() {
            i.url = resolve(&i.url, cwd);
        }
        let mut d = GraphDef { ins, desc: String::new(), out: None, frames: None, threads: 0, pix_fmt: None };
        let mut vf = None;
        for (k, v) in rest {
            match k.as_str() {
                "-filter_complex_threads" => d.threads = v.parse().map_err(|_| format!("{k} {v}"))?,
                "-filter_complex_script" => {
                    d.desc = std::fs::read_to_string(resolve(&v, cwd)).map_err(|e| format!("{v} を読めません: {e}"))?
                }
                "-filter_complex" => d.desc = v,
                "-map" => d.out = Some(v.trim_start_matches('[').trim_end_matches(']').to_string()),
                "-frames:v" => d.frames = Some(v.parse().map_err(|_| format!("{k} {v}"))?),
                "-vf" => vf = Some(v),
                _ => return Err(format!("知らない引数 {k}")),
            }
        }
        if let Some(vf) = vf {
            if !d.desc.is_empty() || d.ins.len() != 1 {
                return Err("-vf と -filter_complex は一緒に使えません".into());
            }
            d.desc = format!("[0:v]{vf}[vf_out]");
            d.out = Some("vf_out".into());
        } else if d.desc.is_empty() {
            d.desc = "[0:v]null[vf_out]".into();
            d.out = Some("vf_out".into());
        }
        Ok(d)
    }
}

/// 相対パスを cwd から見たものに(ffmpeg を cwd で起こしたのと同じに)
pub fn resolve(p: &str, cwd: &str) -> String {
    let path = Path::new(p);
    if cwd.is_empty() || path.is_absolute() || p.contains("://") || p.starts_with("pipe:") {
        p.to_string()
    } else {
        Path::new(cwd).join(path).to_string_lossy().into_owned()
    }
}

struct Src {
    ctx: *mut ff::AVFilterContext,
    dec: usize,
    open: bool,
}

/// 組み上がったグラフ(このスレッドで使う)
pub struct Graph {
    g: *mut ff::AVFilterGraph,
    srcs: Vec<Src>,
    sink: *mut ff::AVFilterContext,
    decs: Vec<Option<Decoder>>,
    left: Option<i64>,
}

impl Drop for Graph {
    fn drop(&mut self) {
        unsafe { ff::avfilter_graph_free(&mut self.g) }
        self.decs.clear();
    }
}

struct InOut(*mut ff::AVFilterInOut);
impl Drop for InOut {
    fn drop(&mut self) {
        unsafe { ff::avfilter_inout_free(&mut self.0) }
    }
}

fn filter(name: &str) -> R<*const ff::AVFilter> {
    let c = cs(name);
    let f = unsafe { ff::avfilter_get_by_name(c.as_ptr()) };
    if f.is_null() { Err(format!("フィルタ {name} がありません")) } else { Ok(f) }
}

fn rat(r: ff::AVRational) -> String {
    format!("{}/{}", r.num, r.den)
}

impl Graph {
    pub fn build(def: &GraphDef, cwd: &str, allow_hw: bool) -> R<Graph> {
        with_cwd(cwd, || Graph::build_in(def, allow_hw))
    }

    fn build_in(def: &GraphDef, allow_hw: bool) -> R<Graph> {
        unsafe {
            // 入力はグラフが使うものだけ開く(音声だけの入力など)
            let decs = def.ins.iter().map(|_| None).collect();
            let mut gr = Graph { g: ff::avfilter_graph_alloc(), srcs: vec![], sink: ptr::null_mut(), decs, left: def.frames };
            let g = gr.g;
            (*g).nb_threads = def.threads;
            let desc = cs(&def.desc);
            let mut ins = InOut(ptr::null_mut());
            let mut outs = InOut(ptr::null_mut());
            check(ff::avfilter_graph_parse2(g, desc.as_ptr(), &mut ins.0, &mut outs.0), "フィルタの解析")?;
            // 入口: "N:v" / "N" → 入力 N の映像
            let mut io = ins.0;
            let mut n = 0;
            while !io.is_null() {
                let name = if (*io).name.is_null() { String::new() } else { CStr::from_ptr((*io).name).to_string_lossy().into_owned() };
                let idx: usize = match name.split(':').collect::<Vec<_>>().as_slice() {
                    [i] | [i, "v"] | [i, "v", "0"] => i.parse().map_err(|_| format!("知らない入口 [{name}]"))?,
                    _ => return Err(format!("知らない入口 [{name}]")),
                };
                let slot = gr.decs.get_mut(idx).ok_or_else(|| format!("入力 {idx} がありません"))?;
                if slot.is_none() {
                    *slot = Some(Decoder::open(def.ins[idx].clone(), allow_hw)?);
                }
                let i = slot.as_ref().unwrap().info;
                let mut args = format!("video_size={}x{}:pix_fmt={}:time_base={}:pixel_aspect={}", i.w, i.h, i.fmt, rat(i.tb),
                                       if i.sar.num > 0 { rat(i.sar) } else { "0/1".into() });
                if i.fr.num > 0 && i.fr.den > 0 {
                    args += &format!(":frame_rate={}", rat(i.fr));
                }
                args += &format!(":colorspace={}:range={}", i.space, i.range);
                let mut ctx = ptr::null_mut();
                let (an, aa) = (cs(&format!("in{n}")), cs(&args));
                check(ff::avfilter_graph_create_filter(&mut ctx, filter("buffer")?, an.as_ptr(), aa.as_ptr(), ptr::null_mut(), g),
                      &format!("入口 [{name}] {args}"))?;
                check(ff::avfilter_link(ctx, 0, (*io).filter_ctx, (*io).pad_idx as u32), "入口をつなぐ")?;
                gr.srcs.push(Src { ctx, dec: idx, open: true });
                io = (*io).next;
                n += 1;
            }
            // 出口: 指定のラベル(ほかの出口は捨てる)
            let mut io = outs.0;
            let mut found = false;
            let mut k = 0;
            while !io.is_null() {
                let name = if (*io).name.is_null() { String::new() } else { CStr::from_ptr((*io).name).to_string_lossy().into_owned() };
                let mine = !found && def.out.as_deref().is_none_or(|o| o == name);
                let mut last = (*io).filter_ctx;
                let mut pad = (*io).pad_idx as u32;
                let mut ctx = ptr::null_mut();
                if mine {
                    if let Some(pf) = &def.pix_fmt {
                        let (fname, fa) = (cs("outfmt"), cs(&format!("pix_fmts={pf}")));
                        let mut fc = ptr::null_mut();
                        check(ff::avfilter_graph_create_filter(&mut fc, filter("format")?, fname.as_ptr(), fa.as_ptr(), ptr::null_mut(), g), "形式")?;
                        check(ff::avfilter_link(last, pad, fc, 0), "出口をつなぐ")?;
                        last = fc;
                        pad = 0;
                    }
                    let nm = cs("out");
                    check(ff::avfilter_graph_create_filter(&mut ctx, filter("buffersink")?, nm.as_ptr(), ptr::null(), ptr::null_mut(), g), "出口")?;
                    gr.sink = ctx;
                    found = true;
                } else {
                    let nm = cs(&format!("null{k}"));
                    check(ff::avfilter_graph_create_filter(&mut ctx, filter("nullsink")?, nm.as_ptr(), ptr::null(), ptr::null_mut(), g), "出口")?;
                }
                check(ff::avfilter_link(last, pad, ctx, 0), "出口をつなぐ")?;
                io = (*io).next;
                k += 1;
            }
            if !found {
                return Err(format!("出口 {:?} がありません", def.out));
            }
            check(ff::avfilter_graph_config(g, ptr::null_mut()), "フィルタの組み立て")?;
            Ok(gr)
        }
    }

    /// デコードを先に始めておく
    pub fn prime(&mut self) {
        for d in self.decs.iter_mut().flatten() {
            d.start();
        }
    }

    /// 次の絵(None = 終わり)。足りない入口に、入力から1コマずつ足しながら回す(ffmpeg の CLI と同じ)
    pub fn pull(&mut self) -> R<Option<Frame>> {
        if self.left == Some(0) {
            return Ok(None);
        }
        let out = Frame::new();
        unsafe {
            loop {
                let r = ff::av_buffersink_get_frame(self.sink, out.0);
                if r >= 0 {
                    if let Some(l) = self.left.as_mut() {
                        *l -= 1;
                    }
                    return Ok(Some(out));
                }
                if r == EOF {
                    return Ok(None);
                }
                if r != EAGAIN {
                    return Err(format!("フィルタ: {}", errstr(r)));
                }
                // いちばん絵を欲しがっている入口の入力から1コマ
                let mut best: Option<(usize, u32)> = None;
                for (i, s) in self.srcs.iter().enumerate() {
                    if s.open {
                        let n = ff::av_buffersrc_get_nb_failed_requests(s.ctx);
                        if best.is_none_or(|b| n > b.1) {
                            best = Some((i, n));
                        }
                    }
                }
                let Some((bi, _)) = best else {
                    return Err("フィルタが止まりました(入力がすべて終わったのに絵が出ません)".into());
                };
                let di = self.srcs[bi].dec;
                let fr = self.decs[di].as_mut().unwrap().recv()?;
                for s in self.srcs.iter_mut().filter(|s| s.open && s.dec == di) {
                    match &fr {
                        Some(f) => {
                            let r = ff::av_buffersrc_add_frame_flags(s.ctx, f.0, ff::AV_BUFFERSRC_FLAG_KEEP_REF as i32);
                            if r == EOF {
                                s.open = false;
                            } else {
                                check(r, "フィルタに絵を渡す")?;
                            }
                        }
                        None => {
                            ff::av_buffersrc_add_frame_flags(s.ctx, ptr::null_mut(), 0);
                            s.open = false;
                        }
                    }
                }
            }
        }
    }
}

/// グラフを裏のスレッドで回し、できた絵を先読みする
pub struct Runner {
    rx: Option<Receiver<R<Option<Frame>>>>,
    th: Option<JoinHandle<()>>,
}

impl Runner {
    pub fn spawn(def: GraphDef, cwd: String, allow_hw: bool, pool: usize) -> Runner {
        let (tx, rx) = sync_channel(pool);
        let th = std::thread::spawn(move || {
            let mut g = match Graph::build(&def, &cwd, allow_hw) {
                Ok(g) => g,
                Err(e) => {
                    let _ = tx.send(Err(e));
                    return;
                }
            };
            g.prime();
            loop {
                let r = g.pull();
                let end = !matches!(r, Ok(Some(_)));
                if tx.send(r).is_err() || end {
                    break;
                }
            }
        });
        Runner { rx: Some(rx), th: Some(th) }
    }

    pub fn recv(&mut self) -> R<Option<Frame>> {
        match &self.rx {
            Some(rx) => rx.recv().unwrap_or_else(|_| Err("フィルタのスレッドが止まりました".into())),
            None => Ok(None),
        }
    }
}

impl Drop for Runner {
    fn drop(&mut self) {
        self.rx = None; // 送れなくなったスレッドは終わる
        if let Some(th) = self.th.take() {
            let _ = th.join();
        }
    }
}
