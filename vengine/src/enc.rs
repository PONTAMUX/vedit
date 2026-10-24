//! エンコード(ffmpeg の `-f rawvideo -i - {enc の引数} 出力.mp4` と同じ)。裏のスレッドで、絵を受け取ってテロップ等のフィルタ → エンコード → mp4 へ。
use crate::av::*;
use crate::graph::with_cwd;
use ffmpeg_sys_next as ff;
use std::ptr;
use std::sync::mpsc::{Receiver, SyncSender, sync_channel};
use std::thread::JoinHandle;

pub struct EncDef {
    vf: Option<String>,
    frames: Option<i64>,
    codec: String,
    tag: Option<String>,
    opts: Vec<(String, String)>,
}

impl EncDef {
    pub fn from_args(args: &[String]) -> R<EncDef> {
        let mut d = EncDef { vf: None, frames: None, codec: String::new(), tag: None, opts: vec![] };
        let mut pix = None;
        let mut i = 0;
        while i < args.len() {
            let k = args[i].as_str();
            if k == "-an" {
                i += 1; // 値を取らない
                continue;
            }
            let v = args.get(i + 1).cloned().ok_or_else(|| format!("引数 {k} の値がありません"))?;
            i += 2;
            match k {
                "-vf" | "-filter:v" => d.vf = Some(v),
                "-frames:v" | "-vframes" => d.frames = Some(v.parse().map_err(|_| format!("{k} {v}"))?),
                "-c:v" | "-vcodec" | "-codec:v" => d.codec = v,
                "-tag:v" => d.tag = Some(v),
                "-pix_fmt" => pix = Some(v),
                _ if k.starts_with('-') => d.opts.push((k[1..].trim_end_matches(":v").to_string(), v)),
                _ => return Err(format!("知らない引数 {k}")),
            }
        }
        if d.codec.is_empty() {
            return Err("エンコーダーの指定(-c:v)がありません".into());
        }
        if let Some(p) = pix {
            d.vf = Some(match d.vf {
                Some(vf) => format!("{vf},format={p}"),
                None => format!("format={p}"),
            });
        }
        Ok(d)
    }
}

/// フィルタを通した絵と、その時刻の単位など(エンコーダーを開くのに使う)
struct Out {
    f: Frame,
    tb: ff::AVRational,
    fr: ff::AVRational,
    sar: ff::AVRational,
}

/// フィルタ(テロップ焼きなど)と NVENC は別々のスレッドで回す(ffmpeg と同じ。片方を待つ間にもう片方が進む)
pub struct Enc {
    pub tx: Option<SyncSender<Frame>>,
    fth: Option<JoinHandle<R<()>>>,
    eth: Option<JoinHandle<R<i64>>>,
}

impl Enc {
    /// W×H の yuv420p を rate で受け取る。cwd はテロップのファイルを探す場所
    pub fn spawn(def: EncDef, w: i32, h: i32, rate: ff::AVRational, cwd: String, out: String, pool: usize) -> Enc {
        let (tx, rx) = sync_channel::<Frame>(pool);
        let (otx, orx) = sync_channel::<Out>(pool);
        let vf = def.vf.clone();
        let limit = def.frames.unwrap_or(i64::MAX);
        let fth = std::thread::spawn(move || filter_loop(vf, limit, w, h, rate, &cwd, rx, otx));
        let eth = std::thread::spawn(move || encode_loop(def, limit, &out, orx));
        Enc { tx: Some(tx), fth: Some(fth), eth: Some(eth) }
    }

    /// 送るのをやめて、書き終わるのを待つ → 書いたコマ数
    pub fn finish(&mut self) -> R<i64> {
        self.tx = None;
        let f = self.fth.take().map(|t| t.join().unwrap_or_else(|_| Err("書き出しのフィルタのスレッドが落ちました".into())));
        let e = self.eth.take().map(|t| t.join().unwrap_or_else(|_| Err("エンコードのスレッドが落ちました".into())));
        f.unwrap_or(Ok(()))?;
        e.unwrap_or_else(|| Err("エンコードは終わっています".into()))
    }
}

impl Drop for Enc {
    fn drop(&mut self) {
        self.tx = None;
        if let Some(th) = self.fth.take() {
            let _ = th.join();
        }
        if let Some(th) = self.eth.take() {
            let _ = th.join();
        }
    }
}

struct G(*mut ff::AVFilterGraph);
impl Drop for G {
    fn drop(&mut self) {
        unsafe { ff::avfilter_graph_free(&mut self.0) }
    }
}
struct C(*mut ff::AVCodecContext);
impl Drop for C {
    fn drop(&mut self) {
        unsafe { ff::avcodec_free_context(&mut self.0) }
    }
}
struct O(*mut ff::AVFormatContext, bool);
impl Drop for O {
    fn drop(&mut self) {
        unsafe {
            if !self.0.is_null() {
                if self.1 {
                    ff::avio_closep(&mut (*self.0).pb);
                }
                ff::avformat_free_context(self.0);
            }
        }
    }
}
struct P(*mut ff::AVPacket);
impl Drop for P {
    fn drop(&mut self) {
        unsafe { ff::av_packet_free(&mut self.0) }
    }
}

fn mk(g: *mut ff::AVFilterGraph, name: &str, inst: &str, args: Option<&str>) -> R<*mut ff::AVFilterContext> {
    let (n, i) = (cs(name), cs(inst));
    let a = args.map(cs);
    let mut ctx = ptr::null_mut();
    unsafe {
        let f = ff::avfilter_get_by_name(n.as_ptr());
        check(ff::avfilter_graph_create_filter(&mut ctx, f, i.as_ptr(), a.as_ref().map_or(ptr::null(), |a| a.as_ptr()), ptr::null_mut(), g),
              &format!("フィルタ {name}"))?;
    }
    Ok(ctx)
}

struct State {
    c: C,
    o: O,
    pkt: P,
    etb: ff::AVRational,
    n: i64,
    hwf: Option<HwFrames>, // NVENC へは GPU のメモリに上げてから渡す
}

struct HwFrames(*mut ff::AVBufferRef);
impl Drop for HwFrames {
    fn drop(&mut self) {
        unsafe { ff::av_buffer_unref(&mut self.0) }
    }
}

fn filter_loop(vf: Option<String>, limit: i64, w: i32, h: i32, rate: ff::AVRational, cwd: &str, rx: Receiver<Frame>, otx: SyncSender<Out>) -> R<()> {
    unsafe {
        let (_g, src, sink) = with_cwd(cwd, || -> R<_> {
            // フィルタ: buffer → (vf) → buffersink
            let g = G(ff::avfilter_graph_alloc());
            (*g.0).nb_threads = 1;
            let tb = q(rate.den, rate.num);
            let src = mk(g.0, "buffer", "in", Some(&format!("video_size={w}x{h}:pix_fmt={}:time_base={}/{}:pixel_aspect=0/1:frame_rate={}/{}",
                                                           ff::AVPixelFormat::AV_PIX_FMT_YUV420P as i32, tb.num, tb.den, rate.num, rate.den)))?;
            let sink = mk(g.0, "buffersink", "out", None)?;
            match &vf {
                Some(vf) => {
                    let mut ins = ff::avfilter_inout_alloc();
                    let mut outs = ff::avfilter_inout_alloc();
                    (*outs).name = ff::av_strdup(cs("in").as_ptr());
                    (*outs).filter_ctx = src;
                    (*ins).name = ff::av_strdup(cs("out").as_ptr());
                    (*ins).filter_ctx = sink;
                    let d = cs(vf);
                    let r = ff::avfilter_graph_parse_ptr(g.0, d.as_ptr(), &mut ins, &mut outs, ptr::null_mut());
                    ff::avfilter_inout_free(&mut ins);
                    ff::avfilter_inout_free(&mut outs);
                    check(r, "書き出しのフィルタの解析")?;
                }
                None => {
                    check(ff::avfilter_link(src, 0, sink, 0), "フィルタをつなぐ")?;
                }
            }
            check(ff::avfilter_graph_config(g.0, ptr::null_mut()), "書き出しのフィルタの組み立て")?;
            Ok((g, src, sink))
        })?;
        let (tb, fr, sar) = (ff::av_buffersink_get_time_base(sink), ff::av_buffersink_get_frame_rate(sink), ff::av_buffersink_get_sample_aspect_ratio(sink));
        let (mut n_in, mut n_out, mut eof) = (0i64, 0i64, false);
        loop {
            // フィルタから出た絵をエンコーダーのスレッドへ
            loop {
                let fo = Frame::new();
                let r = ff::av_buffersink_get_frame(sink, fo.0);
                if r == EAGAIN || r == EOF {
                    break;
                }
                check(r, "書き出しのフィルタ")?;
                if n_out >= limit {
                    continue;
                }
                n_out += 1;
                if otx.send(Out { f: fo, tb, fr, sar }).is_err() {
                    return Ok(()); // エンコーダーが止まった(理由はそちらで)
                }
            }
            if eof {
                return Ok(());
            }
            // 次の絵(送り手が閉じたら終わり)
            match rx.recv() {
                Ok(mut f) if n_in < limit => {
                    let fm = f.fm();
                    fm.pts = n_in;
                    fm.duration = 1;
                    // 生の絵で渡したのと同じに(色の情報は vf の setparams で付ける)
                    fm.color_range = ff::AVColorRange::AVCOL_RANGE_UNSPECIFIED;
                    fm.colorspace = ff::AVColorSpace::AVCOL_SPC_UNSPECIFIED;
                    fm.color_primaries = ff::AVColorPrimaries::AVCOL_PRI_UNSPECIFIED;
                    fm.color_trc = ff::AVColorTransferCharacteristic::AVCOL_TRC_UNSPECIFIED;
                    fm.chroma_location = ff::AVChromaLocation::AVCHROMA_LOC_UNSPECIFIED;
                    fm.sample_aspect_ratio = q(0, 1);
                    ff::av_frame_remove_side_data(f.0, ff::AVFrameSideDataType::AV_FRAME_DATA_DISPLAYMATRIX);
                    check(ff::av_buffersrc_add_frame_flags(src, f.0, 0), "書き出しのフィルタに絵を渡す")?;
                    n_in += 1;
                }
                Ok(_) => {}
                Err(_) => {
                    check(ff::av_buffersrc_add_frame_flags(src, ptr::null_mut(), 0), "書き出しのフィルタを閉じる")?;
                    eof = true;
                }
            }
        }
    }
}

fn encode_loop(def: EncDef, limit: i64, out: &str, rx: Receiver<Out>) -> R<i64> {
    unsafe {
        let mut st: Option<State> = None;
        for o in rx.iter() {
            let fo = o.f;
            if st.as_ref().map_or(0, |s| s.n) >= limit {
                continue;
            }
            if st.is_none() {
                st = Some(open(&def, o.fr, o.sar, &fo, out)?);
            }
            let s = st.as_mut().unwrap();
            if (*fo.0).pts != ff::AV_NOPTS_VALUE {
                (*fo.0).pts = ff::av_rescale_q((*fo.0).pts, o.tb, s.etb);
            }
            (*fo.0).duration = 0;
            (*fo.0).pict_type = ff::AVPictureType::AV_PICTURE_TYPE_NONE;
            let r = match &s.hwf {
                Some(fc) => {
                    let g = Frame::new();
                    check(ff::av_hwframe_get_buffer(fc.0, g.0, 0), "GPU のメモリ")?;
                    check(ff::av_hwframe_transfer_data(g.0, fo.0, 0), "GPU へ送る")?;
                    check(ff::av_frame_copy_props(g.0, fo.0), "絵の情報")?;
                    ff::avcodec_send_frame(s.c.0, g.0)
                }
                None => ff::avcodec_send_frame(s.c.0, fo.0),
            };
            check(r, "エンコード")?;
            s.n += 1;
            drain(s)?;
        }
        let Some(mut s) = st else {
            return Err("書き出す絵が1つもありません".into());
        };
        check(ff::avcodec_send_frame(s.c.0, ptr::null()), "エンコードの終わり")?;
        drain(&mut s)?;
        check(ff::av_write_trailer(s.o.0), "mp4 の書き終わり")?;
        Ok(s.n)
    }
}

/// 最初の絵の形でエンコーダーと mp4 を開く
unsafe fn open(def: &EncDef, fr: ff::AVRational, sar: ff::AVRational, f: &Frame, out: &str) -> R<State> {
    unsafe {
        let name = cs(&def.codec);
        let codec = ff::avcodec_find_encoder_by_name(name.as_ptr());
        if codec.is_null() {
            return Err(format!("エンコーダー {} がありません", def.codec));
        }
        let c = C(ff::avcodec_alloc_context3(codec));
        let e = &mut *c.0;
        // NVENC には GPU のメモリの絵を渡す(CPU のメモリで渡すと NVENC が毎コマ自分で写し、その間エンコーダーが止まる。ffmpeg の GPU 合成と同じ形)
        let mut hwf = None;
        if def.codec.ends_with("_nvenc") && std::env::var_os("VENGINE_NO_HWENC").is_none() {
            if let Some(dev) = crate::input::hw_device() {
                let fc = HwFrames(ff::av_hwframe_ctx_alloc(dev));
                let fctx = &mut *((*fc.0).data as *mut ff::AVHWFramesContext);
                fctx.format = ff::AVPixelFormat::AV_PIX_FMT_CUDA;
                fctx.sw_format = std::mem::transmute::<i32, ff::AVPixelFormat>(f.f().format);
                fctx.width = f.f().width;
                fctx.height = f.f().height;
                if ff::av_hwframe_ctx_init(fc.0) >= 0 {
                    e.hw_frames_ctx = ff::av_buffer_ref(fc.0);
                    hwf = Some(fc);
                }
            }
        }
        let fr = if fr.num > 0 && fr.den > 0 { fr } else { q(30, 1) };
        e.width = f.f().width;
        e.height = f.f().height;
        e.pix_fmt = if hwf.is_some() { ff::AVPixelFormat::AV_PIX_FMT_CUDA } else { std::mem::transmute::<i32, ff::AVPixelFormat>(f.f().format) };
        e.sample_aspect_ratio = sar;
        e.time_base = q(fr.den, fr.num);
        e.framerate = fr;
        e.color_range = f.f().color_range;
        e.colorspace = f.f().colorspace;
        e.color_primaries = f.f().color_primaries;
        e.color_trc = f.f().color_trc;
        e.chroma_sample_location = f.f().chroma_location;
        // mp4
        let mut oc = ptr::null_mut();
        let path = cs(out);
        check(ff::avformat_alloc_output_context2(&mut oc, ptr::null(), ptr::null(), path.as_ptr()), &format!("出力 {out}"))?;
        let mut o = O(oc, false);
        if (*(*oc).oformat).flags & ff::AVFMT_GLOBALHEADER as i32 != 0 {
            e.flags |= ff::AV_CODEC_FLAG_GLOBAL_HEADER as i32;
        }
        let mut d = Dict::new();
        let mut md = Dict::new();
        for (k, v) in &def.opts {
            // コーデックの設定でないもの(movflags など)は mp4 側へ
            let kc = cs(k);
            let isc = !ff::av_opt_find(c.0 as *mut _, kc.as_ptr(), ptr::null(), 0, ff::AV_OPT_SEARCH_CHILDREN as i32).is_null()
                || !ff::av_opt_find((*c.0).priv_data, kc.as_ptr(), ptr::null(), 0, 0).is_null();
            if isc { d.set(k, v) } else { md.set(k, v) }
        }
        check(ff::avcodec_open2(c.0, codec, &mut d.0), &format!("エンコーダー {} を開けません", def.codec))?;
        let left = d.keys();
        if !left.is_empty() {
            return Err(format!("エンコーダーが知らない設定 {left:?}"));
        }
        let s = ff::avformat_new_stream(oc, ptr::null());
        check(ff::avcodec_parameters_from_context((*s).codecpar, c.0), "出力の設定")?;
        (*s).time_base = (*c.0).time_base;
        (*s).avg_frame_rate = fr;
        if let Some(t) = &def.tag {
            let b = t.as_bytes();
            if b.len() == 4 {
                (*(*s).codecpar).codec_tag = u32::from_le_bytes([b[0], b[1], b[2], b[3]]);
            }
        }
        if (*(*oc).oformat).flags & ff::AVFMT_NOFILE as i32 == 0 {
            check(ff::avio_open(&mut (*oc).pb, path.as_ptr(), ff::AVIO_FLAG_WRITE as i32), &format!("書けません {out}"))?;
            o.1 = true;
        }
        check(ff::avformat_write_header(oc, &mut md.0), "mp4 の書き始め")?;
        let left = md.keys();
        if !left.is_empty() {
            return Err(format!("知らない設定 {left:?}"));
        }
        let etb = (*c.0).time_base;
        Ok(State { c, o, pkt: P(ff::av_packet_alloc()), etb, n: 0, hwf })
    }
}

/// エンコーダーから出たものを mp4 へ
unsafe fn drain(s: &mut State) -> R<()> {
    unsafe {
        loop {
            let r = ff::avcodec_receive_packet(s.c.0, s.pkt.0);
            if r == EAGAIN || r == EOF {
                return Ok(());
            }
            check(r, "エンコード")?;
            let st = *(*s.o.0).streams;
            ff::av_packet_rescale_ts(s.pkt.0, s.etb, (*st).time_base);
            (*s.pkt.0).stream_index = 0;
            let r = ff::av_interleaved_write_frame(s.o.0, s.pkt.0);
            check(r, "mp4 へ書く")?;
        }
    }
}
